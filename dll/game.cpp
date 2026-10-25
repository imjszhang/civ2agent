#include "game.h"
#include "jsonutil.h"
#include "types.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::uint8_t* g_base = nullptr;
std::string g_hash;
std::string g_block;
bool g_hash_ok = false;
bool g_limits_ok = false;
void* g_last_dialog = nullptr;
void* g_last_notice = nullptr;
bool g_dialog_announced = false;

// What the last refresh saw, so events report only what the player learns:
// their own units lost or hurt, cities they lost or took, their research.
// Keyed by id, not slot, so a reused slot is not mistaken for the old unit.
struct ShadowUnit {
  int index = 0;
  int hp = 0;
};
struct ShadowCity {
  int owner = -1;
};
std::map<int, ShadowUnit> g_own_units;
std::map<int, ShadowCity> g_cities;
int g_shadow_research = -1;
bool g_shadow_ready = false;
std::vector<std::string> g_events;

using FnCanMove = int(__cdecl*)(int);
using FnGetSq = MapSquare*(__cdecl*)(int, int);
using FnStr = char*(__cdecl*)(int);
using FnYear = int(__cdecl*)(int);

FnCanMove g_can = nullptr;
FnGetSq g_square = nullptr;
FnStr g_string = nullptr;
FnYear g_year_fn = nullptr;

enum class PendingKind { None, EndTurn, FoundCity, Move, Goto, Order, SkipIntro, Produce, Rates };

struct Pending {
  PendingKind kind = PendingKind::None;
  int unit = -1;
  int unit_id = 0;
  int x = 0;
  int y = 0;
  int spent = 0;
  int orders = 0;
  int active = -1;
  int cities = 0;
  int target_x = 0;
  int target_y = 0;
  int turn = 0;
  int year = 0;
  DWORD posted = 0;
  DWORD last_wake = 0;
};

Pending g_pending;
bool g_turn_inflight = false;
int g_inflight_turn = 0;
int g_inflight_year = 0;

constexpr DWORD kNoEffectMs = 1500;
constexpr DWORD kRepostEndTurnMs = 2500;

template <typename T>
T* At(std::uint32_t va) {
  return reinterpret_cast<T*>(g_base + (va - kImageBase));
}

bool Readable(const void* p, size_t n) {
  if (!p || n == 0) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  DWORD prot = mbi.Protect & 0xFF;
  if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
  if (mbi.Protect & PAGE_GUARD) return false;
  auto start = reinterpret_cast<std::uintptr_t>(p);
  auto end = start + n;
  auto region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return end <= region_end;
}

void LogFile(const char* line) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string dir = path;
  auto slash = dir.find_last_of("\\/");
  if (slash != std::string::npos) dir.resize(slash + 1);
  dir += "civ2agent.log";
  FILE* f = nullptr;
  fopen_s(&f, dir.c_str(), "a");
  if (!f) return;
  fputs(line, f);
  fputc('\n', f);
  fclose(f);
}

std::string Sha256File(const char* path) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  std::string out;
  if (!CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
    CloseHandle(file);
    return {};
  }
  if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
    CryptReleaseContext(prov, 0);
    CloseHandle(file);
    return {};
  }
  BYTE buf[1 << 15];
  DWORD got = 0;
  BOOL ok = TRUE;
  while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got) {
    if (!CryptHashData(hash, buf, got, 0)) {
      ok = FALSE;
      break;
    }
  }
  if (ok) {
    BYTE dig[32];
    DWORD n = 32;
    if (CryptGetHashParam(hash, HP_HASHVAL, dig, &n, 0)) {
      static const char* hexd = "0123456789abcdef";
      out.resize(64);
      for (DWORD i = 0; i < n; ++i) {
        out[i * 2] = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0xF];
      }
    }
  }
  CryptDestroyHash(hash);
  CryptReleaseContext(prov, 0);
  CloseHandle(file);
  return out;
}

bool HashAllowed(const std::string& h) {
  return h == "1516bd707462413bcacab0b00007474311532676379a16c29effc70e62838ad2" ||
         h == "b4d05d8755ab4e8813bbc00157b2b8a4604abe8b44955c3c4f325acffdcd3763";
}

bool LimitsAllowed() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string ini = path;
  auto slash = ini.find_last_of("\\/");
  if (slash != std::string::npos) ini.resize(slash + 1);
  ini += "Civ2UIALauncher.ini";
  FILE* f = nullptr;
  fopen_s(&f, ini.c_str(), "r");
  if (!f) return true;
  char line[256];
  bool experimental = false;
  bool ok = true;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "[Experimental]")) experimental = true;
    else if (line[0] == '[') experimental = false;
    if (experimental && strncmp(line, "bUnitsLimit=", 12) == 0) {
      int v = atoi(line + 12);
      if (v != 0) ok = false;
    }
  }
  fclose(f);
  return ok;
}

int SehCan(int unit, int* ret) {
  __try {
    *ret = g_can(unit);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehYear(int turn, int* ret) {
  __try {
    *ret = g_year_fn(turn);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

MapSquare* SehSquare(int x, int y, int* fault) {
  __try {
    return g_square(x, y);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *fault = 1;
    return nullptr;
  }
}

const char* SehString(int index) {
  __try {
    return g_string(index);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// The game's own dialog answer handlers. They act on *kVaPopup and are meant
// to run inside that dialog's message loop, where a click runs them too.
using FnDlgId = void(__cdecl*)(int);
using FnDlgRow = void(__cdecl*)(int, int);
using FnDlgVoid = void(__cdecl*)();
// thiscall (ecx = dialog; record, redraw) called as fastcall with a dummy edx.
using FnDlgSelect = void(__fastcall*)(void*, int, const void*, int);

template <typename Fn>
Fn GameFn(std::uint32_t va) {
  return reinterpret_cast<Fn>(g_base + (va - kImageBase));
}

int SehDlgButton(int id) {
  __try {
    GameFn<FnDlgId>(kVaDlgButton)(id);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehDlgChooseRow(int row) {
  __try {
    GameFn<FnDlgRow>(kVaDlgChooseRow)(0, row);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

// What a double click on a row of the dialog's own list box does (0x5A53B8),
// minus the mouse tracking.
int SehDlgSelectRow(void* dlg, const void* record) {
  __try {
    GameFn<FnDlgSelect>(kVaDlgSelectRow)(dlg, 0, record, 1);
    *reinterpret_cast<std::uint32_t*>(static_cast<char*>(dlg) + kDlgFlags) |= kDlgChoose;
    GameFn<FnDlgVoid>(kVaDlgClose)();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehDlgSysListDone() {
  __try {
    GameFn<FnDlgVoid>(kVaDlgSysListDone)();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

Game* G() { return At<Game>(kVaGame); }
Unit* Units() { return At<Unit>(kVaUnits); }
City* Cities() { return At<City>(kVaCities); }
MapHeader* MapH() { return At<MapHeader>(kVaMapHeader); }
UnitType* Types() { return At<UnitType>(kVaUnitTypes); }
RulesCivilize* Rules() { return At<RulesCivilize>(kVaRules); }

CivHead* CivAt(int i) {
  return reinterpret_cast<CivHead*>(g_base + (kVaCivs - kImageBase) + i * kCivStride);
}

int Human() {
  auto* p = At<int>(kVaHumanCiv);
  if (!Readable(p, sizeof(int))) return -1;
  return *p;
}

bool MatchLive() {
  auto* map = MapH();
  auto* game = G();
  if (!Readable(map, sizeof(MapHeader)) || !Readable(game, sizeof(Game))) return false;
  if (map->size_x < 1 || map->size_y < 1 || map->size_x > 512 || map->size_y > 512) return false;
  int human = Human();
  if (human < 0 || human > 7) return false;
  if (game->total_units < 0 || game->total_units > kUnitSlots) return false;
  if (game->total_cities > kCitySlots) return false;
  if (game->turn < 0) return false;
  if (game->total_units == 0 && game->total_cities == 0) return false;
  return true;
}

void* ValidDialog(void* dlg) {
  if (!Readable(dlg, 0x2F4)) return nullptr;
  int buttons = *reinterpret_cast<int*>(static_cast<char*>(dlg) + kDlgNumButtons);
  if (buttons < 0 || buttons > 8) return nullptr;
  return dlg;
}

// The popup on top of the game's popup stack, which the dialog handlers act on.
void* TopPopup() {
  auto** slot = At<void*>(kVaPopup);
  if (!Readable(slot, sizeof(void*))) return nullptr;
  return ValidDialog(*slot);
}

bool NoWait(void* dlg) {
  return (*reinterpret_cast<std::uint32_t*>(static_cast<char*>(dlg) + kDlgFlags) & kDlgNoWait) != 0;
}

// The dialog the game waits on: the topmost popup that runs its own loop.
// Advice popups (no-wait) can sit above it without holding the game.
void* DialogPtr() {
  void* top = TopPopup();
  if (!top || !NoWait(top)) return top;
  auto* count = At<int>(kVaPopupCount);
  auto** stack = At<void*>(kVaPopupStack);
  if (!Readable(count, sizeof(int)) || *count < 1 || *count > 16 || !Readable(stack, 17 * sizeof(void*)))
    return nullptr;
  for (int i = *count; i >= 1; --i) {
    void* dlg = ValidDialog(stack[i]);
    if (dlg && !NoWait(dlg)) return dlg;
  }
  return nullptr;
}

// An advice popup on top that the game does not wait on.
void* NoticePtr() {
  void* top = TopPopup();
  return top && NoWait(top) ? top : nullptr;
}

std::string ReadGameText(const char* s, int cap) {
  if (!s || !Readable(s, 1)) return {};
  int n = 0;
  while (n < cap && Readable(s + n, 1) && s[n]) ++n;
  if (!n) return {};
  int wlen = MultiByteToWideChar(CP_ACP, 0, s, n, nullptr, 0);
  if (wlen <= 0) return std::string(s, s + n);
  std::wstring w(wlen, 0);
  MultiByteToWideChar(CP_ACP, 0, s, n, w.data(), wlen);
  int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, nullptr, 0, nullptr, nullptr);
  std::string out(ulen, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, out.data(), ulen, nullptr, nullptr);
  return out;
}

std::string DialogOptionsJson(void* dlg);
std::string DialogEditsJson(void* dlg);
std::string ModalScreenJson();

std::string DialogHeadJson(void* dlg) {
  auto* base = static_cast<char*>(dlg);
  const char* title = *reinterpret_cast<char**>(base + kDlgTitle);
  std::string body;
  auto* line = *reinterpret_cast<char**>(base + kDlgFirstText);
  int lines = 0;
  while (line && lines < 12 && Readable(line, 0x20)) {
    const char* text = *reinterpret_cast<char**>(line + 8);
    body += ReadGameText(text, 240);
    body.push_back('\n');
    line = *reinterpret_cast<char**>(line + 0x1C);
    ++lines;
  }
  return "\"title\":" + JsonString(ReadGameText(title, 120)) + ",\"text\":" + JsonString(body);
}

// Every dialog report carries the same numbered options that act respond
// picks from, so an index read from modal_open or an event is valid there.
std::string DialogJson() {
  void* dlg = DialogPtr();
  if (dlg) return "{" + DialogHeadJson(dlg) + "," + DialogOptionsJson(dlg) + DialogEditsJson(dlg) + "}";
  std::string screen = ModalScreenJson();
  return screen.empty() ? "null" : screen;
}

// Advice the game shows without waiting for an answer.
std::string NoticeJson() {
  void* dlg = NoticePtr();
  return dlg ? "{" + DialogHeadJson(dlg) + "}" : "null";
}

void PushEvent(const std::string& json_object) { g_events.push_back(json_object); }

std::string CityName(const City& c) {
  char buf[17] = {};
  memcpy(buf, c.name, 16);
  return ReadGameText(buf, 16);
}

std::string TechName(int tech) {
  if (tech < 0 || tech >= 100) return {};
  auto* rule = Rules() + tech;
  if (!Readable(rule, sizeof(RulesCivilize))) return {};
  return ReadGameText(SehString(rule->text_index), 80);
}

// A modal page that is not a popup. Pedia is the advance page after a
// discovery; View is the foreign-ministry report and the same viewer.
enum class ModalKind { None, Pedia, View };

int ReadIntAt(std::uint32_t va) {
  auto* p = At<int>(va);
  return Readable(p, sizeof(int)) ? *p : 0;
}

ModalKind CurrentModal() {
  if (!MatchLive()) return ModalKind::None;
  if (ReadIntAt(kVaViewWindow + kViewOpen) > 0) return ModalKind::View;
  if (ReadIntAt(kVaPediaStage) != 0 || ReadIntAt(kVaPediaLive) != 0) return ModalKind::Pedia;
  return ModalKind::None;
}

std::string CloseOnlyDialog(const std::string& title, const std::string& text) {
  return "{\"title\":" + JsonString(title) + ",\"text\":" + JsonString(text) +
         ",\"kind\":\"controls\",\"via\":\"game\",\"options\":[{\"index\":0,\"text\":" + JsonString("关闭") + "}]}";
}

std::string CivText(std::uint32_t va, int civ);

const char* GovernmentName(int government) {
  static const char* names[] = {"无政府", "专制", "君主", "共产", "原教旨", "共和", "民主"};
  if (government < 0 || government >= 7) return "";
  return names[government];
}

std::string PediaText(int tech) {
  std::string out = "允许：";
  int shown = 0;
  for (int i = 0; i < 100 && shown < 12; ++i) {
    auto* rule = Rules() + i;
    if (!Readable(rule, sizeof(RulesCivilize))) break;
    if (rule->preq[0] != tech && rule->preq[1] != tech) continue;
    std::string name = TechName(i);
    if (name.empty()) continue;
    if (shown) out += "、";
    out += name;
    ++shown;
  }
  return shown ? out : std::string{};
}

int PediaTech() {
  int page = ReadIntAt(kVaPediaWindow + kPediaPage);
  if (page >= 0 && page < 256) {
    int tech = ReadIntAt(kVaPediaWindow + kPediaEntries + static_cast<unsigned>(page) * 4);
    if (tech >= 0 && tech < 100) return tech;
  }
  int item = ReadIntAt(kVaPediaItem);
  return item >= 0 && item < 100 ? item : -1;
}

std::string PediaBody(int tech) {
  auto* rule = Rules() + tech;
  std::string text;
  if (Readable(rule, sizeof(RulesCivilize))) {
    std::string pre;
    for (int p : {static_cast<int>(rule->preq[0]), static_cast<int>(rule->preq[1])}) {
      if (p < 0) continue;
      std::string name = TechName(p);
      if (name.empty()) continue;
      if (!pre.empty()) pre += "、";
      pre += name;
    }
    if (!pre.empty()) text = "先决条件：" + pre;
  }
  std::string allows = PediaText(tech);
  if (!allows.empty()) {
    if (!text.empty()) text += "\n";
    text += allows;
  }
  return text;
}

std::string PediaJson() {
  int kind = ReadIntAt(kVaPediaKind);
  int tech = PediaTech();
  if ((kind == 1 || kind == 8) && tech >= 0) {
    std::string title = TechName(tech);
    if (title.empty()) title = "科技";
    return CloseOnlyDialog(title, PediaBody(tech));
  }
  return CloseOnlyDialog("百科", "");
}

std::string ViewJson() {
  int civ = ReadIntAt(kVaIntelCiv);
  if (civ < 0 || civ >= kCivSlots) return CloseOnlyDialog("窗口", "");
  auto* head = CivAt(civ);
  std::string text;
  std::string name = CivText(kVaCivName, civ);
  if (!name.empty()) text += name;
  if (Readable(head, sizeof(CivHead))) {
    const char* gov = GovernmentName(head->government);
    if (*gov) {
      if (!text.empty()) text += "\n";
      text += gov;
    }
    if (!text.empty()) text += "\n";
    text += "国库：" + std::to_string(head->gold);
    int units = 0;
    int cities = 0;
    auto* game = G();
    int nu = (std::min)(static_cast<int>(game->total_units), kUnitSlots);
    int nc = (std::min)(static_cast<int>(game->total_cities), kCitySlots);
    for (int i = 0; i < nu; ++i)
      if (Units()[i].id != 0 && Units()[i].civ == civ) ++units;
    for (int i = 0; i < nc; ++i)
      if (Cities()[i].id != 0 && Cities()[i].owner == static_cast<std::uint8_t>(civ)) ++cities;
    text += "\n军事：" + std::to_string(units);
    text += "\n城市：" + std::to_string(cities);
    std::string research = TechName(head->researching);
    if (!research.empty()) text += "\n正在研究：" + research;
  }
  return CloseOnlyDialog("外交部·情报报告", text);
}

std::string ModalScreenJson() {
  switch (CurrentModal()) {
    case ModalKind::Pedia:
      return PediaJson();
    case ModalKind::View:
      return ViewJson();
    case ModalKind::None:
      return {};
  }
  return {};
}

int SehPediaClose() {
  __try {
    GameFn<void(__cdecl*)()>(kVaPediaClose)();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehPediaDismiss() {
  __try {
    GameFn<void(__cdecl*)()>(kVaPediaDismiss)();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehViewClose() {
  __try {
    GameFn<void(__cdecl*)()>(kVaViewButtonClose)();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

std::string CloseModalScreen(ModalKind kind) {
  if (kind == ModalKind::Pedia) {
    if (SehPediaClose()) return ErrJson("game_fault", "关闭科技页时出错");
    if (auto* stage = At<int>(kVaPediaStage); Readable(stage, sizeof(int))) *stage = 0;
    if (SehPediaDismiss()) return ErrJson("game_fault", "关闭科技页时出错");
    return OkJson(std::string("\"index\":0,\"text\":") + JsonString("关闭") + ",\"via\":\"game\",\"changed\":true,\"dialog\":" +
                  DialogJson());
  }
  if (kind == ModalKind::View) {
    if (SehViewClose()) return ErrJson("game_fault", "关闭情报页时出错");
    return OkJson(std::string("\"index\":0,\"text\":") + JsonString("关闭") + ",\"via\":\"game\",\"changed\":true,\"dialog\":" +
                  DialogJson());
  }
  return ErrJson("no_dialog", "没有打开的界面");
}

const char* SehCivText(std::uint32_t va, int civ) {
  __try {
    return GameFn<const char*(__cdecl*)(int)>(va)(civ);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// A name the game shows for the civ: its name, its leader's, the title.
std::string CivText(std::uint32_t va, int civ) { return ReadGameText(SehCivText(va, civ), 60); }

void RefreshShadow() {
  if (!MatchLive()) {
    g_shadow_ready = false;
    return;
  }
  auto* game = G();
  int human = Human();
  auto* units = Units();
  auto* cities = Cities();
  int nu = (std::min)(static_cast<int>(game->total_units), kUnitSlots);
  int nc = (std::min)(static_cast<int>(game->total_cities), kCitySlots);
  std::map<int, ShadowUnit> own;
  for (int i = 0; i < nu; ++i) {
    if (units[i].id != 0 && units[i].civ == human) own[units[i].id] = {i, units[i].hp_lost};
  }
  std::map<int, ShadowCity> seen;
  for (int i = 0; i < nc; ++i) {
    if (cities[i].id != 0) seen[cities[i].id] = {cities[i].owner};
  }
  int research = CivAt(human)->researching;
  if (g_shadow_ready) {
    for (const auto& [id, before] : g_own_units) {
      auto now = own.find(id);
      if (now == own.end()) {
        PushEvent(std::string("{\"type\":\"unit_lost\",\"index\":") + std::to_string(before.index) +
                  ",\"id\":" + std::to_string(id) + "}");
      } else if (now->second.hp > before.hp) {
        PushEvent(std::string("{\"type\":\"unit_damaged\",\"index\":") + std::to_string(now->second.index) +
                  ",\"id\":" + std::to_string(id) + ",\"hp_lost\":" + std::to_string(now->second.hp) + "}");
      }
    }
    // Only changes of hands the player is part of; the rest happen out of view.
    for (int i = 0; i < nc; ++i) {
      const City& c = cities[i];
      auto before = c.id ? g_cities.find(c.id) : g_cities.end();
      if (before == g_cities.end() || before->second.owner == c.owner) continue;
      std::string where = ",\"name\":" + JsonString(CityName(c)) + ",\"x\":" + std::to_string(c.x) +
                          ",\"y\":" + std::to_string(c.y);
      if (c.owner == human) {
        PushEvent(std::string("{\"type\":\"city_captured\",\"index\":") + std::to_string(i) + ",\"id\":" +
                  std::to_string(c.id) + where + ",\"previous_owner\":" + std::to_string(before->second.owner) + "}");
      } else if (before->second.owner == human) {
        PushEvent(std::string("{\"type\":\"city_lost\"") + where + ",\"owner\":" + std::to_string(c.owner) + "}");
      }
    }
    if (research != g_shadow_research) {
      PushEvent(std::string("{\"type\":\"research\",\"tech\":") + std::to_string(research) +
                ",\"name\":" + JsonString(TechName(research)) + "}");
    }
  }
  g_own_units.swap(own);
  g_cities.swap(seen);
  g_shadow_research = research;
  g_shadow_ready = true;
}

bool Blocked(std::string& err) {
  if (!g_hash_ok) {
    err = ErrJson("version_mismatch", "civ2.exe 哈希不在允许列表");
    return true;
  }
  if (!g_limits_ok) {
    err = ErrJson("unsupported_limits", "bUnitsLimit 已打开，单位表长度不是 2048");
    return true;
  }
  return false;
}

bool RequireMatch(std::string& err) {
  if (Blocked(err)) return false;
  if (!MatchLive()) {
    err = ErrJson("not_in_game", "当前不在对局中");
    return false;
  }
  auto* game = G();
  if (game->multi_type != 0) {
    err = ErrJson("unsupported", "v1 只处理单人游戏");
    return false;
  }
  return true;
}

int NormalizeDelta(int delta, int width) {
  if (width <= 0) return delta;
  while (delta > width / 2) delta -= width;
  while (delta < -(width / 2)) delta += width;
  return delta;
}

int DirectionTo(int ux, int uy, int tx, int ty) {
  auto* dxs = At<std::int8_t>(kVaPfdx);
  auto* dys = At<std::int8_t>(kVaPfdy);
  if (!Readable(dxs, 8) || !Readable(dys, 8)) return -1;
  auto* map = MapH();
  int widths[2] = {map->size_x, map->size_x * 2};
  int nwidth = map->flat ? 1 : 2;
  for (int w = 0; w < nwidth; ++w) {
    int dx = map->flat ? (tx - ux) : NormalizeDelta(tx - ux, widths[w]);
    int dy = ty - uy;
    for (int dir = 0; dir < 8; ++dir) {
      if (dxs[dir] == dx && dys[dir] == dy) return dir;
    }
  }
  return -1;
}

int MovesLeft(int unit);

std::string UnitBrief(int index) {
  auto& u = Units()[index];
  return std::string("\"index\":") + std::to_string(index) + ",\"x\":" + std::to_string(u.x) +
         ",\"y\":" + std::to_string(u.y) + ",\"orders\":" + std::to_string(u.orders) +
         ",\"moves_left\":" + std::to_string(MovesLeft(index));
}

bool HumansTurn() {
  auto* game = G();
  return !(game->some_civ >= 0 && game->some_civ <= 7 && game->some_civ != Human());
}

bool OwnUnit(int index, std::string& err) {
  auto* game = G();
  if (index < 0 || index >= game->total_units) {
    err = ErrJson("illegal", "单位下标超出本局单位数");
    return false;
  }
  auto& u = Units()[index];
  if (u.id == 0) {
    err = ErrJson("illegal", "这个单位槽是空的");
    return false;
  }
  if (u.civ != Human()) {
    err = ErrJson("illegal", "这不是己方单位");
    return false;
  }
  if (!HumansTurn()) {
    err = ErrJson("not_your_turn", "当前不是人类玩家的行动阶段");
    return false;
  }
  return true;
}

bool ContainsCjk(const std::string& utf8) {
  const auto* s = reinterpret_cast<const unsigned char*>(utf8.data());
  for (size_t i = 0; i < utf8.size();) {
    if (s[i] < 0x80) {
      ++i;
      continue;
    }
    if ((s[i] & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      unsigned cp = ((s[i] & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
      if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
      i += 3;
      continue;
    }
    ++i;
  }
  return false;
}

bool AllowedMenuByte(unsigned char c) {
  if (c < 0x80) {
    return c == ' ' || c == '.' || c == '(' || c == ')' || c == ',' || (c >= '0' && c <= '9') ||
         (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }
  return false;
}

void CollectMenuLabels(void* dlg, std::string& title, std::vector<std::string>& options) {
  auto* base = static_cast<char*>(dlg);
  const char* title_a = *reinterpret_cast<const char**>(base + kDlgTitle);
  title = ReadGameText(title_a, 120);
  const auto* block = reinterpret_cast<const unsigned char*>(*reinterpret_cast<const char**>(base + 0x25C));
  if (!Readable(block, 16)) block = reinterpret_cast<const unsigned char*>(title_a);
  if (!Readable(block, 16)) return;
  const int span = 0x180;
  if (!Readable(block, span)) return;
  for (int i = 0; i < span;) {
    if (block[i] < 0x81 || block[i] > 0xFE) {
      ++i;
      continue;
    }
    int j = i;
    int cjk = 0;
    bool ok = true;
    while (j < span && block[j] != 0) {
      if (block[j] < 0x80) {
        if (!AllowedMenuByte(block[j])) {
          ok = false;
          break;
        }
        ++j;
        continue;
      }
      if (j + 1 >= span || block[j + 1] < 0x40 || block[j + 1] > 0xFE || block[j + 1] == 0x7F) {
        ok = false;
        break;
      }
      ++cjk;
      j += 2;
    }
    if (ok && j < span && block[j] == 0 && cjk >= 2 && (j - i) <= 40) {
      std::string text = ReadGameText(reinterpret_cast<const char*>(block + i), j - i);
      if (text != title && ContainsCjk(text)) options.push_back(text);
      i = j + 1;
    } else {
      ++i;
    }
  }
}

struct MenuFind {
  const char* title_a;
  HWND hwnd;
  int best_area;
};

BOOL CALLBACK FindMenuWindow(HWND hwnd, LPARAM lp) {
  auto* find = reinterpret_cast<MenuFind*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
  char buf[160] = {};
  GetWindowTextA(hwnd, buf, 160);
  if (strcmp(buf, find->title_a) != 0) return TRUE;
  RECT rect{};
  GetWindowRect(hwnd, &rect);
  int area = (rect.right - rect.left) * (rect.bottom - rect.top);
  if (area < 4000 || area > 900 * 700) return TRUE;
  if (!find->hwnd || area < find->best_area) {
    find->hwnd = hwnd;
    find->best_area = area;
  }
  return TRUE;
}

struct MenuRows {
  std::vector<std::pair<int, HWND>> rows;
};

BOOL CALLBACK CollectMenuRows(HWND hwnd, LPARAM lp) {
  auto* rows = reinterpret_cast<MenuRows*>(lp);
  RECT rect{};
  GetWindowRect(hwnd, &rect);
  int height = rect.bottom - rect.top;
  int width = rect.right - rect.left;
  if (height >= 20 && height <= 48 && width >= 80) rows->rows.push_back({rect.top, hwnd});
  return TRUE;
}

// How act respond answers one option: through the game's own handler for that
// control (read from the dialog object), or with a real click (Legacy).
enum class Answer { Legacy, Button, Row, ListRow, SysListRow };

struct MenuChoice {
  std::string text;
  HWND hwnd = nullptr;
  int list_index = -1;
  int list_id = -1;
  bool line = false;
  Answer answer = Answer::Legacy;
  int control_id = -1;            // Button: id passed to the button handler
  int std_type = -1;              // Button: 0 OK, 1 Help, 2 Cancel; -1 for captioned buttons
  int row = -1;                   // Row: index in the dialog's row chain
  const char* record = nullptr;   // rows: the chain record
  bool disabled = false;
};

// Civ2UIA draws icons inside list rows from markup like "10#644F00:3#"
// and right-aligns what follows '|'. Report the text a player reads.
std::string CleanListText(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '|') {
      out += ' ';
      continue;
    }
    if (text[i] == '#') {
      size_t j = i + 1;
      while (j < text.size() && isxdigit(static_cast<unsigned char>(text[j]))) ++j;
      size_t colon = j;
      if (colon > i + 1 && colon < text.size() && text[colon] == ':') {
        size_t k = colon + 1;
        while (k < text.size() && isdigit(static_cast<unsigned char>(text[k]))) ++k;
        if (k > colon + 1 && k < text.size() && text[k] == '#') {
          i = k;
          continue;
        }
      }
    }
    out += text[i];
  }
  return out;
}

struct ListLine {
  const char* record;
  int id;
  std::string text;
};

std::vector<ListLine> ReadDialogList(void* dlg, int* selected) {
  std::vector<ListLine> lines;
  if (selected) *selected = -1;
  auto* base = static_cast<char*>(dlg);
  const char* head = *reinterpret_cast<const char**>(base + kDlgListHead);
  const char* current = *reinterpret_cast<const char**>(base + kDlgListSelected);
  const char* rec = head;
  while (rec && lines.size() < 64 && Readable(rec, kListNext + sizeof(char*))) {
    const char* text = *reinterpret_cast<const char* const*>(rec + kListText);
    std::string label = CleanListText(ReadGameText(text, 160));
    if (label.empty()) break;
    if (selected && rec == current) *selected = static_cast<int>(lines.size());
    lines.push_back({rec, *reinterpret_cast<const int*>(rec + kListId), label});
    rec = *reinterpret_cast<const char* const*>(rec + kListNext);
  }
  return lines;
}

std::vector<MenuChoice> BuildMenuChoices(void* dlg, bool* is_list) {
  std::vector<MenuChoice> choices;
  if (is_list) *is_list = false;
  std::string title;
  std::vector<std::string> labels;
  CollectMenuLabels(dlg, title, labels);
  std::vector<ListLine> list = ReadDialogList(dlg, nullptr);
  if (!list.empty()) {
    if (is_list) *is_list = true;
    for (size_t i = 0; i < list.size(); ++i) {
      MenuChoice choice;
      choice.text = list[i].text;
      choice.list_index = static_cast<int>(i);
      choice.list_id = list[i].id;
      choices.push_back(choice);
    }
    labels.clear();
  }
  auto* base = static_cast<char*>(dlg);
  int num_buttons = *reinterpret_cast<int*>(base + kDlgNumButtons);
  std::vector<std::string> button_labels;
  if (num_buttons > 0 && num_buttons <= 6) {
    auto** texts = reinterpret_cast<char**>(base + kDlgButtonTexts);
    for (int i = 0; i < num_buttons; ++i) {
      const char* t = Readable(texts + i, sizeof(char*)) ? texts[i] : nullptr;
      button_labels.push_back(ReadGameText(t, 80));
    }
  }
  std::vector<std::string> line_labels;
  for (const std::string& label : labels) {
    bool is_button = false;
    for (const std::string& button : button_labels) {
      if (label == button) is_button = true;
    }
    if (!is_button) line_labels.push_back(label);
  }
  const char* title_a = *reinterpret_cast<const char**>(base + kDlgTitle);
  MenuFind find{title_a, nullptr, 0};
  EnumWindows(FindMenuWindow, reinterpret_cast<LPARAM>(&find));
  MenuRows rows;
  if (find.hwnd) EnumChildWindows(find.hwnd, CollectMenuRows, reinterpret_cast<LPARAM>(&rows));
  std::sort(rows.rows.begin(), rows.rows.end(), [](const std::pair<int, HWND>& a, const std::pair<int, HWND>& b) {
    if (a.first != b.first) return a.first < b.first;
    RECT ra{}, rb{};
    GetWindowRect(a.second, &ra);
    GetWindowRect(b.second, &rb);
    return ra.left < rb.left;
  });
  std::vector<HWND> lines;
  std::vector<HWND> buttons;
  for (const auto& row : rows.rows) {
    RECT rect{};
    GetWindowRect(row.second, &rect);
    int height = rect.bottom - rect.top;
    if (height <= 32) lines.push_back(row.second);
    else buttons.push_back(row.second);
  }
  std::sort(buttons.begin(), buttons.end(), [](HWND a, HWND b) {
    RECT ra{}, rb{};
    GetWindowRect(a, &ra);
    GetWindowRect(b, &rb);
    return ra.left < rb.left;
  });
  if (list.empty()) {
    int line_count = static_cast<int>(lines.size());
    if (line_count > static_cast<int>(line_labels.size())) line_count = static_cast<int>(line_labels.size());
    for (int i = 0; i < line_count; ++i) {
      MenuChoice choice;
      choice.text = line_labels[i];
      choice.hwnd = lines[i];
      choice.line = true;
      choices.push_back(choice);
    }
  } else if (lines.size() == list.size()) {
    // List rows are real controls laid out column by column, in list order.
    // Posted arrow keys do not move the highlight, so rows are clicked too.
    std::sort(lines.begin(), lines.end(), [](HWND a, HWND b) {
      RECT ra{}, rb{};
      GetWindowRect(a, &ra);
      GetWindowRect(b, &rb);
      if (ra.left != rb.left) return ra.left < rb.left;
      return ra.top < rb.top;
    });
    for (size_t i = 0; i < list.size(); ++i) {
      choices[i].hwnd = lines[i];
      choices[i].line = true;
    }
  }
  // Buttons without a caption in ButtonTexts are the dialog's own OK and
  // Cancel, laid out after the captioned ones.
  static const char* const kUnnamed[] = {"确定", "取消"};
  int bottom = 0;
  for (HWND b : buttons) {
    RECT rect{};
    GetWindowRect(b, &rect);
    bottom = (std::max)(bottom, static_cast<int>(rect.top));
  }
  size_t unnamed = 0;
  for (size_t i = 0; i < buttons.size(); ++i) {
    MenuChoice choice;
    choice.hwnd = buttons[i];
    if (i < button_labels.size()) {
      choice.text = button_labels[i];
    } else {
      RECT rect{};
      GetWindowRect(buttons[i], &rect);
      if (rect.top < bottom - 4 || unnamed >= 2) continue;
      choice.text = kUnnamed[unnamed++];
    }
    choices.push_back(choice);
  }
  return choices;
}

// Rows and buttons straight from the dialog object, in screen order, each
// answered by the handler a click on it would run. Returns false for dialogs
// that need more than one call to answer (checkboxes, two lists) or show
// icon rows; those keep the click path.
bool BuildNativeChoices(void* dlg, std::vector<MenuChoice>& out) {
  auto* base = static_cast<char*>(dlg);
  if (!Readable(base, 0x2F4)) return false;
  auto flags = *reinterpret_cast<std::uint32_t*>(base + kDlgFlags);
  int lines = *reinterpret_cast<int*>(base + kDlgNumLines);
  int items = *reinterpret_cast<int*>(base + kDlgNumListItems);
  int std_buttons = *reinterpret_cast<int*>(base + kDlgNumButtonsStd);
  int custom_buttons = *reinterpret_cast<int*>(base + kDlgNumButtons);
  if (flags & (kDlgCheckboxes | kDlgTwoLists)) return false;
  if (lines == 0 && items > 1) return false;
  if (lines < 0 || std_buttons < 0 || std_buttons > 3 || custom_buttons < 0 || custom_buttons > 6) return false;

  std::vector<MenuChoice> rows;
  if (lines > 0) {
    Answer kind = !(flags & kDlgListbox)        ? Answer::Row
                  : (flags & kDlgSystemListbox) ? Answer::SysListRow
                                                : Answer::ListRow;
    // Radio rows stop at 40; the dialog's own list shows the rows of its page.
    int limit = kind == Answer::Row ? (std::min)(lines, kDlgMaxRows) : 512;
    int page = *reinterpret_cast<int*>(base + kDlgListPage);
    const char* rec = *reinterpret_cast<const char**>(base + kDlgListHead);
    for (int chain = 0; rec && chain < limit && Readable(rec, kListNext + sizeof(char*)); ++chain) {
      if (kind == Answer::Row || *reinterpret_cast<const int*>(rec + kListPage) == page) {
        MenuChoice choice;
        choice.text = CleanListText(ReadGameText(*reinterpret_cast<const char* const*>(rec + kListText), 160));
        choice.list_id = *reinterpret_cast<const int*>(rec + kListId);
        choice.answer = kind;
        choice.row = chain;
        choice.record = rec;
        // Only radio rows are disabled controls; the list box ignores the bit.
        choice.disabled = kind == Answer::Row && (*reinterpret_cast<const std::uint32_t*>(rec + kListFlags) & kListDisabled);
        if (!choice.text.empty()) rows.push_back(choice);
      }
      rec = *reinterpret_cast<const char* const*>(rec + kListNext);
    }
  }

  // Standard buttons (OK, Help, Cancel) come first, then the captioned ones;
  // control ids follow that order, the screen shows them by left edge.
  auto** captions = At<const char*>(kVaPopupCaptions);
  auto** texts = reinterpret_cast<const char**>(base + kDlgButtonTexts);
  std::vector<std::pair<int, MenuChoice>> placed;
  for (int i = 0; i < std_buttons + custom_buttons; ++i) {
    const int* layout = reinterpret_cast<const int*>(base + kDlgButtonLayout + i * 8);
    const char* caption = nullptr;
    if (i < std_buttons) {
      int type = layout[0];
      if (type >= 0 && type <= 2 && Readable(captions + type, sizeof(char*))) caption = captions[type];
    } else {
      caption = texts[i - std_buttons];
    }
    MenuChoice choice;
    choice.text = ReadGameText(caption, 80);
    choice.answer = Answer::Button;
    choice.control_id = kButtonIdBase + i;
    if (i < std_buttons) choice.std_type = layout[0];
    if (!choice.text.empty()) placed.push_back({layout[1], choice});
  }
  std::stable_sort(placed.begin(), placed.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  out = rows;
  for (const auto& p : placed) out.push_back(p.second);
  return true;
}

struct DialogChoices {
  std::vector<MenuChoice> choices;
  bool native = false;
  bool is_list = false;
  int default_index = -1;  // option highlighted in the dialog, lists only
};

// The options that act respond picks from, so an index read from a report
// is valid when answering the same dialog.
DialogChoices ChoicesFor(void* dlg) {
  DialogChoices out;
  if (BuildNativeChoices(dlg, out.choices)) {
    out.native = true;
    const char* current = *reinterpret_cast<const char**>(static_cast<char*>(dlg) + kDlgListSelected);
    for (size_t i = 0; i < out.choices.size(); ++i) {
      if (out.choices[i].answer == Answer::Button) continue;
      out.is_list = true;
      if (out.choices[i].record == current) out.default_index = static_cast<int>(i);
    }
    return out;
  }
  out.choices = BuildMenuChoices(dlg, &out.is_list);
  if (out.is_list) ReadDialogList(dlg, &out.default_index);
  return out;
}

// The list box control of the dialog's current page, for system list boxes.
HWND SysListWindow(void* dlg) {
  auto* base = static_cast<char*>(dlg);
  int page = *reinterpret_cast<int*>(base + kDlgListPage);
  if (page < 0 || page > 1) return nullptr;
  const char* control = *reinterpret_cast<const char* const*>(base + kDlgSysListControls + page * 4);
  if (!Readable(control, kControlHwnd + sizeof(HWND))) return nullptr;
  HWND hwnd = *reinterpret_cast<const HWND*>(control + kControlHwnd);
  return IsWindow(hwnd) ? hwnd : nullptr;
}

// Runs on the game thread inside the dialog's own loop, so the handler does
// what a click would and the game closes the dialog itself.
std::string AnswerNative(void* dlg, const MenuChoice& choice, const std::string& head) {
  // The handlers act on the top popup; an advice popup above would take it.
  if (TopPopup() != dlg) return ErrJson("no_dialog", "对话框上面盖着建议框，稍后再试");
  auto* base = static_cast<char*>(dlg);
  auto flags = *reinterpret_cast<std::uint32_t*>(base + kDlgFlags);
  if ((flags & (kDlgCreated | kDlgCreatedParts)) != (kDlgCreated | kDlgCreatedParts) ||
      !Readable(*reinterpret_cast<void**>(base), 0xD8))
    return ErrJson("no_dialog", "对话框还没画出来，稍后再试");
  if (flags & kDlgClosing) return ErrJson("no_dialog", "对话框正在关闭");
  if (choice.disabled) return ErrJson("illegal", "这一行不可选");
  int fault = 0;
  switch (choice.answer) {
    case Answer::Button:
      fault = SehDlgButton(choice.control_id);
      break;
    case Answer::Row:
      fault = SehDlgChooseRow(choice.row);
      break;
    case Answer::ListRow:
      fault = SehDlgSelectRow(dlg, choice.record);
      break;
    case Answer::SysListRow: {
      HWND list = SysListWindow(dlg);
      if (!list) return ErrJson("no_dialog", "找不到列表控件");
      const char* text = *reinterpret_cast<const char* const*>(choice.record + kListText);
      LRESULT at = SendMessageA(list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text));
      if (at == LB_ERR) return ErrJson("illegal", "列表里没有这一行");
      SendMessageA(list, LB_SETCURSEL, static_cast<WPARAM>(at), 0);
      fault = SehDlgSysListDone();
      break;
    }
    default:
      return ErrJson("illegal", "这个选项不能直接应答");
  }
  if (fault) return ErrJson("game_fault", "对话框处理函数异常");
  return OkJson(head + ",\"via\":\"game\"");
}

// Current text of the dialog's edit boxes (city name, leader name).
std::string DialogEditsJson(void* dlg) {
  auto* base = static_cast<char*>(dlg);
  int edits = *reinterpret_cast<int*>(base + kDlgNumEdits);
  auto flags = *reinterpret_cast<std::uint32_t*>(base + kDlgFlags);
  if (edits <= 0 || edits > 8 || !(flags & kDlgCreatedParts)) return {};
  const char* controls = *reinterpret_cast<const char* const*>(base + kDlgEditControls);
  if (!Readable(controls, edits * kEditControlStride)) return {};
  std::string out = ",\"edits\":[";
  for (int i = 0; i < edits; ++i) {
    HWND hwnd = *reinterpret_cast<const HWND*>(controls + i * kEditControlStride + kControlHwnd);
    char buf[256] = {};
    if (IsWindow(hwnd)) GetWindowTextA(hwnd, buf, sizeof(buf));
    if (i) out += ',';
    out += "{\"index\":" + std::to_string(i) + ",\"text\":" + JsonString(ReadGameText(buf, sizeof(buf) - 1)) + "}";
  }
  return out + "]";
}

HWND DialogWindow(void* dlg) {
  const char* title_a = *reinterpret_cast<const char**>(static_cast<char*>(dlg) + kDlgTitle);
  if (!title_a || !Readable(title_a, 1)) return nullptr;
  MenuFind find{title_a, nullptr, 0};
  EnumWindows(FindMenuWindow, reinterpret_cast<LPARAM>(&find));
  return find.hwnd;
}

void PostKey(HWND hwnd, UINT vk) {
  UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
  LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
  LPARAM up = down | (1u << 30) | (1u << 31);
  PostMessageA(hwnd, WM_KEYDOWN, vk, down);
  if (vk == VK_RETURN || vk == VK_SPACE) PostMessageA(hwnd, WM_CHAR, vk, down);
  PostMessageA(hwnd, WM_KEYUP, vk, up);
}

std::string DialogOptionsJson(void* dlg) {
  DialogChoices dc = ChoicesFor(dlg);
  std::string out = std::string("\"kind\":") + JsonString(dc.is_list ? "list" : "controls");
  // The row highlighted when the dialog opened.
  if (dc.is_list) out += ",\"default\":" + std::to_string(dc.default_index);
  // game: respond calls the dialog's own handler; mouse: it clicks.
  out += std::string(",\"via\":") + JsonString(dc.native ? "game" : "mouse");
  out += ",\"options\":[";
  for (size_t i = 0; i < dc.choices.size(); ++i) {
    const MenuChoice& c = dc.choices[i];
    if (i) out += ',';
    out += std::string("{\"index\":") + std::to_string(i) + ",\"text\":" + JsonString(c.text);
    if (c.record || c.list_id >= 0) out += ",\"id\":" + std::to_string(c.list_id);
    if (c.disabled) out += ",\"disabled\":true";
    out += "}";
  }
  out += "]";
  return out;
}

std::string CityWindowJson();
std::string TaxWindowJson();

// The dialog on screen (null when there is none), in a match or not, and the
// city and tax windows and advice when shown.
std::string DialogStateJson() {
  bool live = MatchLive();
  return OkJson(std::string("\"in_game\":") + (live ? "true" : "false") +
                ",\"city_window\":" + (live ? CityWindowJson() : std::string("null")) +
                ",\"tax_window\":" + (live ? TaxWindowJson() : std::string("null")) + ",\"notice\":" + NoticeJson() +
                ",\"dialog\":" + DialogJson());
}

// UTF-8 to the game's code page. False when a character has no equivalent.
bool ToGameText(const std::string& utf8, std::string& out) {
  out.clear();
  if (utf8.empty()) return true;
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                 nullptr, 0);
  if (wlen <= 0) return false;
  std::wstring w(wlen, 0);
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wlen);
  BOOL lossy = FALSE;
  int alen = WideCharToMultiByte(CP_ACP, 0, w.c_str(), wlen, nullptr, 0, nullptr, &lossy);
  if (alen <= 0 || lossy) return false;
  out.resize(alen);
  WideCharToMultiByte(CP_ACP, 0, w.c_str(), wlen, out.data(), alen, nullptr, nullptr);
  return true;
}

// The edit control's own per-character filter (MSEditBox +0x34, consulted for
// typed characters by 0x5D30E0). Double-byte characters arrive through the
// IME, so only single-byte ones are checked.
bool EditAccepts(const char* control, const std::string& text) {
  if (!Readable(control, kEditFilter + sizeof(void*))) return true;
  using Filter = int(__cdecl*)(int, int);
  auto filter = *reinterpret_cast<Filter const*>(control + kEditFilter);
  int arg = *reinterpret_cast<const int*>(control + 4);
  if (!filter) return true;
  for (unsigned char c : text) {
    if (c >= 0x80) continue;
    int ok = 1;
    __try {
      ok = filter(arg, c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
    }
    if (!ok) return false;
  }
  return true;
}

// Types into the dialog's edit boxes, in order, as a player would before
// pressing OK. Every text is checked first, so a refusal leaves all boxes as
// they were. Each box keeps its own length limit; a mismatch on read-back is
// refused.
std::string SetDialogEdits(void* dlg, const std::vector<std::string>& utf8) {
  auto* base = static_cast<char*>(dlg);
  int edits = *reinterpret_cast<int*>(base + kDlgNumEdits);
  auto flags = *reinterpret_cast<std::uint32_t*>(base + kDlgFlags);
  if (edits <= 0 || edits > 8 || !(flags & kDlgCreatedParts)) return ErrJson("illegal", "这个对话框没有输入框");
  if (utf8.empty() || static_cast<int>(utf8.size()) > edits)
    return ErrJson("illegal", "这个对话框只有 " + std::to_string(edits) + " 个输入框");
  const char* controls = *reinterpret_cast<const char* const*>(base + kDlgEditControls);
  if (!Readable(controls, static_cast<int>(utf8.size()) * kEditControlStride))
    return ErrJson("no_dialog", "输入框还没画出来，稍后再试");
  std::vector<std::pair<HWND, std::string>> typed;
  for (size_t i = 0; i < utf8.size(); ++i) {
    const char* control = controls + i * kEditControlStride;
    HWND hwnd = *reinterpret_cast<const HWND*>(control + kControlHwnd);
    if (!IsWindow(hwnd)) return ErrJson("no_dialog", "输入框还没画出来，稍后再试");
    std::string text;
    if (!ToGameText(utf8[i], text)) return ErrJson("illegal", "输入的文字里有游戏不支持的字符");
    // Typing stops at the box's limit (15 bytes for a city name), but
    // WM_SETTEXT does not, and the game copies the text unbounded into its
    // own buffer. Hold it to what a player could type.
    auto limit = static_cast<size_t>(SendMessageA(hwnd, EM_GETLIMITTEXT, 0, 0));
    if (text.size() > limit)
      return ErrJson("illegal", "输入框最多 " + std::to_string(limit) + " 字节（一个汉字 2 字节）");
    for (unsigned char c : text) {
      if (c < 0x20) return ErrJson("illegal", "输入框不接受控制字符");
    }
    if (!EditAccepts(control, text)) return ErrJson("illegal", "输入框不接受其中的字符");
    typed.push_back({hwnd, text});
  }
  for (const auto& [hwnd, text] : typed) {
    SendMessageA(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
    char back[256] = {};
    GetWindowTextA(hwnd, back, sizeof(back));
    if (text != back) return ErrJson("illegal", "输入框放不下这段文字");
  }
  return {};
}

// Most dialogs are answered through the game's own handler for the chosen
// control. The rest (checkboxes, icon rows) have real controls that ignore
// posted mouse messages; for those the caller gets screen coordinates and
// clicks with the real cursor. Rows take a double click, which selects and
// confirms.
std::string ClickMenuOption(int index, const std::string& text, const std::vector<std::string>* edits) {
  void* dlg = DialogPtr();
  if (!dlg) {
    ModalKind screen = CurrentModal();
    if (screen == ModalKind::None) return ErrJson("no_dialog", "没有打开的界面");
    if (edits && !edits->empty()) return ErrJson("illegal", "这个窗口没有输入框");
    bool close = text.empty() ? index == 0 : text == "关闭";
    if (!close) return ErrJson("illegal", "这个窗口只有「关闭」");
    return CloseModalScreen(screen);
  }
  std::vector<MenuChoice> choices = ChoicesFor(dlg).choices;
  if (!text.empty()) {
    index = -1;
    for (size_t i = 0; i < choices.size(); ++i) {
      if (choices[i].text == text) {
        index = static_cast<int>(i);
        break;
      }
    }
    if (index < 0) return ErrJson("illegal", "界面上没有这个选项");
  }
  if (index < 0 || index >= static_cast<int>(choices.size())) return ErrJson("illegal", "选项下标超出范围");
  const MenuChoice& choice = choices[index];
  std::string head = std::string("\"index\":") + std::to_string(index) + ",\"text\":" + JsonString(choice.text);
  if (edits) {
    if (choice.disabled) return ErrJson("illegal", "这一行不可选");
    std::string refused = SetDialogEdits(dlg, *edits);
    if (!refused.empty()) return refused;
  }
  if (choice.answer != Answer::Legacy) return AnswerNative(dlg, choice, head);
  if (choice.list_index >= 0 && !choice.hwnd) {
    // Without row controls (not drawn yet, or a scrolling list) only the row
    // highlighted at open can be confirmed: Enter takes the live highlight,
    // and the list's recorded row is where it started.
    HWND hwnd = DialogWindow(dlg);
    if (!hwnd) return ErrJson("no_dialog", "找不到对话框窗口");
    int start = -1;
    ReadDialogList(dlg, &start);
    if (choice.list_index != (start < 0 ? 0 : start))
      return ErrJson("illegal", "找不到这一行的控件，只能确认默认行；稍后重新读取 menu 再试");
    PostKey(hwnd, VK_RETURN);
    return OkJson(head + ",\"via\":\"keys\"");
  }
  if (!choice.hwnd || !IsWindow(choice.hwnd)) return ErrJson("no_dialog", "找不到选项控件");
  RECT rect{};
  GetWindowRect(choice.hwnd, &rect);
  HWND top = GetAncestor(choice.hwnd, GA_ROOT);
  return OkJson(head + ",\"click\":{\"x\":" + std::to_string((rect.left + rect.right) / 2) +
                ",\"y\":" + std::to_string((rect.top + rect.bottom) / 2) +
                ",\"hwnd\":" + std::to_string(reinterpret_cast<std::uintptr_t>(top)) +
                ",\"repeats\":" + (choice.line ? "2" : "1") + "}");
}

struct TopPick {
  HWND hwnd = nullptr;
  int area = 0;
};

BOOL CALLBACK PickMainWindow(HWND hwnd, LPARAM lp) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || !GetMenu(hwnd)) return TRUE;
  RECT rect{};
  GetWindowRect(hwnd, &rect);
  int area = (rect.right - rect.left) * (rect.bottom - rect.top);
  auto* pick = reinterpret_cast<TopPick*>(lp);
  if (area > pick->area) {
    pick->hwnd = hwnd;
    pick->area = area;
  }
  return TRUE;
}

BOOL CALLBACK PickMapChild(HWND hwnd, LPARAM lp) {
  char cls[40] = {};
  GetClassNameA(hwnd, cls, sizeof(cls));
  if (strcmp(cls, "MSWindowClass") != 0 || !IsWindowVisible(hwnd)) return TRUE;
  RECT rect{};
  GetWindowRect(hwnd, &rect);
  int area = (rect.right - rect.left) * (rect.bottom - rect.top);
  auto* pick = reinterpret_cast<TopPick*>(lp);
  if (area > pick->area) {
    pick->hwnd = hwnd;
    pick->area = area;
  }
  return TRUE;
}

HWND MainWindow() {
  TopPick pick;
  EnumWindows(PickMainWindow, reinterpret_cast<LPARAM>(&pick));
  return pick.hwnd;
}

HWND MapWindow() {
  HWND main = MainWindow();
  if (!main) return nullptr;
  TopPick pick;
  EnumChildWindows(main, PickMapChild, reinterpret_cast<LPARAM>(&pick));
  return pick.hwnd;
}

constexpr LPARAM kKickEnter = 1;
constexpr LPARAM kKickEscape = 2;

BOOL CALLBACK KickChild(HWND hwnd, LPARAM flags) {
  PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 0);
  PostMessageA(hwnd, WM_LBUTTONUP, 0, 0);
  if (flags & kKickEnter) PostKey(hwnd, VK_RETURN);
  if (flags & kKickEscape) {
    PostKey(hwnd, VK_ESCAPE);
    PostKey(hwnd, VK_SPACE);
  }
  return TRUE;
}

BOOL CALLBACK KickWindow(HWND hwnd, LPARAM flags) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
  PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 0);
  PostMessageA(hwnd, WM_LBUTTONUP, 0, 0);
  if (flags & kKickEnter) PostKey(hwnd, VK_RETURN);
  if (flags & kKickEscape) {
    PostKey(hwnd, VK_ESCAPE);
    PostKey(hwnd, VK_SPACE);
  }
  PostMessageA(hwnd, WM_NULL, 0, 0);
  EnumChildWindows(hwnd, KickChild, flags);
  return TRUE;
}

bool MenuHasStartGame() {
  void* dlg = DialogPtr();
  if (!dlg) return false;
  for (const auto& choice : ChoicesFor(dlg).choices) {
    if (choice.text == "开始新游戏") return true;
  }
  return false;
}

std::string SkipIntroJson(bool skipped) {
  return OkJson(std::string("\"skipped\":") + (skipped ? "true" : "false") +
                ",\"at_menu\":" + (MenuHasStartGame() ? "true" : "false") +
                ",\"in_game\":" + (MatchLive() ? "true" : "false"));
}

struct MenuCommand {
  UINT id = 0;
  bool enabled = false;
};

// Menu items are found by the shortcut shown after the tab, e.g. "b" for
// 建立新城市 or "Ctrl+N" for 结束全部回合. Item captions change with the active
// unit and terrain, the shortcuts do not.
MenuCommand FindCommand(const wchar_t* shortcut) {
  MenuCommand found;
  HWND main = MainWindow();
  HMENU bar = main ? GetMenu(main) : nullptr;
  if (!bar) return found;
  int top = GetMenuItemCount(bar);
  for (int i = 0; i < top; ++i) {
    HMENU sub = GetSubMenu(bar, i);
    if (!sub) continue;
    int count = GetMenuItemCount(sub);
    for (int j = 0; j < count; ++j) {
      wchar_t caption[128] = {};
      if (!GetMenuStringW(sub, j, caption, 128, MF_BYPOSITION)) continue;
      const wchar_t* tab = wcschr(caption, L'\t');
      if (!tab || wcscmp(tab + 1, shortcut) != 0) continue;
      found.id = GetMenuItemID(sub, j);
      UINT state = GetMenuState(sub, j, MF_BYPOSITION);
      found.enabled = (state & (MF_GRAYED | MF_DISABLED)) == 0;
      return found;
    }
  }
  return found;
}

const wchar_t* OrderShortcut(const std::string& order) {
  if (order == "fortify") return L"f";
  if (order == "sleep") return L"s";
  if (order == "road") return L"r";
  if (order == "irrigate") return L"i";
  if (order == "mine") return L"m";
  if (order == "clean") return L"p";
  if (order == "wait") return L"w";
  if (order == "skip") return L"SPACE";
  return nullptr;
}

UINT DirectionKey(int dir) {
  static const UINT kKeys[8] = {VK_NUMPAD9, VK_NUMPAD6, VK_NUMPAD3, VK_NUMPAD2,
                                VK_NUMPAD1, VK_NUMPAD4, VK_NUMPAD7, VK_NUMPAD8};
  return dir >= 0 && dir < 8 ? kKeys[dir] : 0;
}

int StepToward(int ux, int uy, int tx, int ty) {
  auto* dxs = At<std::int8_t>(kVaPfdx);
  auto* dys = At<std::int8_t>(kVaPfdy);
  if (!Readable(dxs, 8) || !Readable(dys, 8)) return -1;
  auto* map = MapH();
  int width = map->size_x * 2;
  auto distance = [&](int x, int y) {
    int dx = map->flat ? (tx - x) : NormalizeDelta(tx - x, width);
    int dy = ty - y;
    return (std::max)(std::abs(dx + dy), std::abs(dx - dy)) / 2;
  };
  int best = -1;
  int best_dist = distance(ux, uy);
  for (int dir = 0; dir < 8; ++dir) {
    int d = distance(ux + dxs[dir], uy + dys[dir]);
    if (d < best_dist) {
      best = dir;
      best_dist = d;
    }
  }
  return best;
}

void SnapshotUnit() {
  auto* game = G();
  g_pending.active = game->active_unit;
  g_pending.cities = game->total_cities;
  int unit = g_pending.unit;
  if (unit < 0 || unit >= kUnitSlots) return;
  auto& u = Units()[unit];
  g_pending.unit_id = u.id;
  g_pending.x = u.x;
  g_pending.y = u.y;
  g_pending.spent = u.move_points;
  g_pending.orders = u.orders;
  g_pending.posted = GetTickCount();
}

void BeginUnitPending(PendingKind kind, int unit, int tx, int ty) {
  g_pending = {};
  g_pending.kind = kind;
  g_pending.unit = unit;
  g_pending.target_x = tx;
  g_pending.target_y = ty;
  SnapshotUnit();
}

bool UnitAlive() {
  int unit = g_pending.unit;
  return unit >= 0 && unit < kUnitSlots && Units()[unit].id != 0 && Units()[unit].id == g_pending.unit_id;
}

bool UnitChanged() {
  auto* game = G();
  if (!UnitAlive()) return true;
  if (game->total_cities != g_pending.cities) return true;
  if (game->active_unit != g_pending.active) return true;
  auto& u = Units()[g_pending.unit];
  return u.x != g_pending.x || u.y != g_pending.y || u.move_points != g_pending.spent ||
         u.orders != g_pending.orders;
}

void KeepPumping(DWORD now) {
  if (now - g_pending.last_wake < 30) return;
  g_pending.last_wake = now;
  HWND main = MainWindow();
  if (main) PostMessageA(main, WM_NULL, 0, 0);
}

// Mid-turn the game ends the turn from 结束全部回合 (Ctrl+N). Once every unit has
// moved that item is greyed and the game waits for Enter on the map instead.
bool PostEndTurn() {
  MenuCommand cmd = FindCommand(L"Ctrl+N");
  if (cmd.id && cmd.enabled) {
    HWND main = MainWindow();
    if (!main) return false;
    PostMessageA(main, WM_COMMAND, cmd.id, 0);
    return true;
  }
  HWND map = MapWindow();
  if (!map) return false;
  PostKey(map, VK_RETURN);
  return true;
}

// The year as the status bar shows it: negative is BC.
int GameYear() { return static_cast<std::int16_t>(G()->year); }

std::string TurnJson() {
  auto* game = G();
  RefreshShadow();
  auto* civ = CivAt(Human());
  return OkJson(std::string("\"turn\":") + std::to_string(game->turn) + ",\"year\":" + std::to_string(GameYear()) +
                ",\"gold\":" + std::to_string(civ->gold));
}

// What the player sees of other civs, decided the way the map draws it
// (tile 0x47C103: city gate 0x47BA1D, unit gate 0x47BE63).
bool TileExploredBy(const MapSquare& sq, int civ) { return (sq.visibility & (1u << civ)) != 0; }

bool CityKnownTo(const City& c, int civ) {
  return (c.known_to & (1u << civ)) != 0 && c.revealed_size[civ] != 0;
}

int StackLink(std::uint16_t link) { return static_cast<std::int16_t>(link); }

int StackHead(int unit);
int DrawnUnit(int head, bool airbase);

// A left click on the unit's tile, as the map's click handler 0x410F77 does
// it. Clicking wakes a sleeping or fortified unit; on a stack it opens the
// units-list popup and returns once that is answered.
int SehClickUnit(int x, int y) {
  __try {
    GameFn<void(__cdecl*)(int)>(kVaLeaveMovingMode)(0);
    GameFn<int(__cdecl*)(int, int)>(kVaSetCursor)(x, y);
    GameFn<void(__cdecl*)()>(kVaActivateAtCursor)();
    // thiscall (map window; x, y) called as fastcall with a dummy edx.
    GameFn<void(__fastcall*)(void*, int, int, int)>(kVaCenterView)(At<char>(kVaMapWindow), 0, x, y);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

bool InStack(int unit) {
  const Unit& u = Units()[unit];
  return StackLink(u.prev_stack) >= 0 || StackLink(u.next_stack) >= 0;
}

// The unit is selected and keys move it (the game is in moving mode).
bool Moving(int unit) {
  auto* selected = At<int>(kVaUnitSelected);
  return G()->active_unit == unit && Readable(selected, sizeof(int)) && *selected != 0;
}

// Selects the unit for a move or an order the way a player does, by clicking
// it. On a stack a click opens the units-list popup, so these ask for
// activate first.
std::string SelectForCommand(int unit) {
  if (Moving(unit)) return {};
  // A click would wake a fortified unit and still fail to select it.
  if (MovesLeft(unit) <= 0) return ErrJson("illegal", "这个单位这回合已经没有移动力");
  if (InStack(unit)) return ErrJson("illegal", "这个单位和别的单位在同一格，先用 activate 选中它");
  const Unit& u = Units()[unit];
  if (SehClickUnit(u.x, u.y)) return ErrJson("game_fault", "选中单位时出错");
  if (!Moving(unit)) return ErrJson("illegal", "点了这个单位也选不中它，它这回合已经不能行动");
  return {};
}

// Other civs' units as the map shows them: one per tile, the one drawn, and
// only what its picture shows. A tile counts when the player has explored it
// and the stack's head carries the player's spotted bit; units in cities are
// not drawn. "!" when the map cannot be read.
std::string ForeignUnitsJson(int human) {
  auto* game = G();
  auto* units = Units();
  auto* types = Types();
  int nu = (std::min)(static_cast<int>(game->total_units), kUnitSlots);
  std::map<std::pair<int, int>, int> tiles;  // tile -> lowest unit on it
  for (int i = 0; i < nu; ++i) {
    if (units[i].id == 0) continue;
    tiles.emplace(std::make_pair(static_cast<int>(units[i].x), static_cast<int>(units[i].y)), i);
  }
  bool damage_shown = (game->custom_features & kFeatureDamage) != 0;
  std::string out;
  for (const auto& [pos, any] : tiles) {
    int head = StackHead(any);
    const Unit& h = units[head];
    if (h.civ == human || !(h.visibility & (1u << human))) continue;
    int fault = 0;
    MapSquare* sq = SehSquare(pos.first, pos.second, &fault);
    if (fault) return "!";
    if (!sq || !TileExploredBy(*sq, human) || (sq->features & kFeatureCity)) continue;
    bool airbase = (sq->features & kFeatureAirbase) == kFeatureAirbase;
    int drawn = DrawnUnit(head, airbase);
    if (drawn < 0) continue;
    const Unit& u = units[drawn];
    std::string uname;
    int hp = 0;
    if (u.type < 62 && Readable(types + u.type, sizeof(UnitType))) {
      uname = ReadGameText(SehString(types[u.type].string_index), 40);
      hp = types[u.type].hp;
    }
    // The shield shows the order letter for orders 1-10, "-" otherwise.
    int orders = u.orders >= 1 && u.orders <= 10 ? u.orders : -1;
    bool stacked = StackLink(u.prev_stack) >= 0 || StackLink(u.next_stack) >= 0;
    if (!out.empty()) out += ',';
    out += "{\"civ\":" + std::to_string(u.civ) + ",\"x\":" + std::to_string(u.x) + ",\"y\":" + std::to_string(u.y) +
           ",\"type\":" + std::to_string(u.type) + ",\"name\":" + JsonString(uname) +
           ",\"orders\":" + std::to_string(orders) +
           ",\"automated\":" + ((u.attributes & kUnitAutomated) ? "true" : "false") +
           ",\"stacked\":" + (stacked ? "true" : "false");
    if (damage_shown && hp > 0) {
      // The health bar, in tenths, as precise as the picture.
      int left = (std::max)(0, hp - static_cast<int>(u.hp_lost));
      out += ",\"health\":" + std::to_string((left * 10 + hp / 2) / hp * 10);
    }
    out += "}";
  }
  return out;
}

// The unit a tile's stack is judged by: walk PrevInStack to its head.
int StackHead(int unit) {
  auto* units = Units();
  for (int steps = 0; steps < kUnitSlots; ++steps) {
    int prev = StackLink(units[unit].prev_stack);
    if (prev < 0 || prev == unit || prev >= kUnitSlots) return unit;
    unit = prev;
  }
  return unit;
}

// The unit the map draws for a stack (0x56B96E): air and sea units over
// ground units, air units on an airbase hidden, the first of equals wins.
int DrawnUnit(int head, bool airbase) {
  auto* units = Units();
  auto* types = Types();
  int best = -1, best_score = -1;
  for (int u = head, steps = 0; u >= 0 && u < kUnitSlots && steps < kUnitSlots;
       u = StackLink(units[u].next_stack), ++steps) {
    int type = units[u].type;
    int domain = type < 62 && Readable(types + type, sizeof(UnitType)) ? types[type].domain : 0;
    int score = domain != 0 ? 2 : 1;
    if (domain == 1 && airbase) score = -1;
    if (score > best_score) {
      best_score = score;
      best = u;
    }
  }
  return best;
}

// Tile features as the civ last saw them; others' changes out of sight stay
// unseen. Units are reported from the unit list, so their bit is dropped.
int KnownFeatures(int x, int y, int civ) {
  auto fn = GameFn<std::uint8_t*(__cdecl*)(int, int, int)>(kVaMapGetCivData);
  std::uint8_t* known = nullptr;
  __try {
    known = fn(x, y, civ);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
  if (!Readable(known, 1)) return -1;
  return *known & ~kFeatureUnit;
}

int MovesLeft(int unit) {
  __try {
    return GameFn<int(__cdecl*)(int)>(kVaUnitMovesLeft)(unit);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// Movement points per whole move, as the unit panel divides them.
int MoveUnit() {
  auto* cosmic = At<std::uint8_t>(kVaCosmic);
  return Readable(cosmic, 1) && *cosmic ? *cosmic : 3;
}

int AdvanceCost(int civ) {
  __try {
    return GameFn<int(__cdecl*)(int)>(kVaGetAdvanceCost)(civ);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// The relation the Foreign Minister shows; null before contact.
const char* TreatyStatus(std::int32_t t) {
  if (!(t & kTreatyContact)) return nullptr;
  if (t & kTreatyAlliance) return "alliance";
  if (t & kTreatyPeace) return "peace";
  if (t & kTreatyCeaseFire) return "ceasefire";
  if (t & kTreatyWar) return "war";
  return "none";
}

// The city window as the player sees it: which city is open (-1 for none),
// and whether the game holds the turn until it is closed.
int OpenCityWindow() {
  auto* win = At<char>(kVaCityWindow);
  if (!Readable(win + kCityWinHidden, sizeof(int))) return -1;
  if (*reinterpret_cast<int*>(win + kCityWinHidden)) return -1;
  int city = *reinterpret_cast<int*>(win + kCityWinCityIndex);
  return city >= 0 && city < kCitySlots ? city : -1;
}

bool CityWindowLocked() {
  auto* lock = At<int>(kVaLockCityWindow);
  return Readable(lock, sizeof(int)) && *lock != 0;
}

std::string CityWindowJson() {
  int city = OpenCityWindow();
  if (city < 0) return "null";
  return std::string("{\"city\":") + std::to_string(city) + ",\"name\":" + JsonString(CityName(Cities()[city])) +
         ",\"modal\":" + (CityWindowLocked() ? "true" : "false") + "}";
}

int SehCityWindowShow(int city) {
  __try {
    // thiscall (window; city) called as fastcall with a dummy edx.
    GameFn<void(__fastcall*)(void*, int, int)>(kVaCityWindowShow)(At<char>(kVaCityWindow), 0, city);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehCityChange() {
  __try {
    GameFn<void(__cdecl*)(int)>(kVaCityChange)(0);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehCityWindowExit() {
  __try {
    GameFn<void(__cdecl*)()>(kVaCityWindowExit)();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

// What a city is building, by the name the game shows.
std::string ProductionName(int building) {
  if (building >= 0) {
    auto* type = Types() + building;
    if (building >= 62 || !Readable(type, sizeof(UnitType))) return {};
    return ReadGameText(SehString(type->string_index), 60);
  }
  int improvement = -building;
  auto* entry = At<std::uint32_t>(kVaImprovements) + improvement * 2;
  if (improvement > 66 || !Readable(entry, 4)) return {};
  return ReadGameText(SehString(static_cast<int>(*entry)), 60);
}

// @PRODUCTION record id to TCity::building.
int BuildingOfRecord(int id) { return id < kProductionImprovementBase ? id : -(id - kProductionImprovementBase); }

// produce, step by step on the game thread: the city window opens, Change runs
// @PRODUCTION (its loop dispatches through our hook, where the row is chosen),
// then the window closes again unless the player had it open.
enum class ProduceStage { Idle, Showing, Opened, Picking, Picked, Changed };
struct ProduceRun {
  ProduceStage stage = ProduceStage::Idle;
  int run = 0;
  int city = -1;
  std::string item;
  int item_id = -1;
  bool was_open = false;
  void* list = nullptr;  // the @PRODUCTION dialog once answered
  std::string answer;    // response decided at the list, when it does not depend on the change
  DWORD at = 0;
};
ProduceRun g_produce;
int g_produce_runs = 0;
constexpr DWORD kProduceStepMs = 250;  // let each step show before the next

std::string ProductionOptionsJson(const std::vector<MenuChoice>& choices) {
  std::string out = "[";
  bool first = true;
  for (const MenuChoice& c : choices) {
    if (c.answer == Answer::Button) continue;
    int building = BuildingOfRecord(c.list_id);
    if (!first) out += ',';
    first = false;
    out += "{\"id\":" + std::to_string(c.list_id) + ",\"building\":" + std::to_string(building) +
           ",\"name\":" + JsonString(ProductionName(building)) + ",\"text\":" + JsonString(c.text) +
           (c.disabled ? ",\"disabled\":true" : "") + "}";
  }
  return out + "]";
}

// The Change list of this city, and not an advice or other popup.
bool IsProductionList(void* dlg, int city) {
  auto* base = static_cast<char*>(dlg);
  auto flags = *reinterpret_cast<std::uint32_t*>(base + kDlgFlags);
  if (!(flags & kDlgListbox) || (flags & (kDlgSystemListbox | kDlgTwoLists))) return false;
  const char* rec = *reinterpret_cast<const char**>(base + kDlgListHead);
  int rows = 0;
  for (; rec && rows < 512 && Readable(rec, kListNext + sizeof(char*)); ++rows) {
    int id = *reinterpret_cast<const int*>(rec + kListId);
    if (id < 0 || id > kProductionImprovementBase + 66) return false;
    rec = *reinterpret_cast<const char* const*>(rec + kListNext);
  }
  if (rows == 0) return false;
  std::string title = ReadGameText(*reinterpret_cast<const char**>(base + kDlgTitle), 120);
  return title.find(CityName(Cities()[city])) != std::string::npos;
}

std::string FinishProduce(const std::string& response) {
  g_pending = {};
  g_produce.stage = ProduceStage::Idle;
  RefreshShadow();
  return response;
}

// A popup the player would have to answer came up mid-way (a shield-loss
// confirmation, advice). Report it as it stands; nothing is answered for the
// agent and the city window is left as it is.
std::string HandOffProduce() {
  g_pending = {};
  g_produce.stage = ProduceStage::Idle;
  return std::string("{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"游戏弹出了对话框，先 respond\",\"city_window\":") +
         CityWindowJson() + ",\"dialog\":" + DialogJson() + "}";
}

void CloseCityWindowIfOpened() {
  if (!g_produce.was_open && !CityWindowLocked() && OpenCityWindow() >= 0) SehCityWindowExit();
}

// rates, as a player does it: open the tax window from the menu, drag each
// slider to its value, press OK. The window keeps its own rules: the
// government's maximum and the locks.
enum class RatesStage { Idle, Opening, Sliding, Closing };
struct RatesRun {
  RatesStage stage = RatesStage::Idle;
  int run = 0;
  int want[3] = {};   // tax, science, lux
  int start[3] = {};  // as the window opened
  int max_rate = -1;
  int moves = 0;
  bool restoring = false;
  std::string refused;  // reason, when the request could not be met
  DWORD at = 0;
};
RatesRun g_rates;
int g_rates_runs = 0;
constexpr DWORD kRatesStepMs = 150;
constexpr int kRatesMaxMoves = 9;  // three passes over the three sliders

char* TaxWindow() {
  auto* open = At<int>(kVaTaxWindowOpen);
  auto** ptr = At<char*>(kVaTaxWindowPtr);
  if (!Readable(open, sizeof(int)) || !*open || !Readable(ptr, sizeof(char*))) return nullptr;
  char* w = *ptr;
  return Readable(w, kTaxWinScienceBar + sizeof(HWND)) ? w : nullptr;
}

int TaxField(char* w, int offset) { return *reinterpret_cast<int*>(w + offset); }

std::string TaxWindowJson() {
  char* w = TaxWindow();
  if (!w) return "null";
  return "{\"max_rate\":" + std::to_string(TaxField(w, kTaxWinMaxRate)) + ",\"tax_rate\":" +
         std::to_string(TaxField(w, kTaxWinTax)) + ",\"science_rate\":" + std::to_string(TaxField(w, kTaxWinScience)) +
         ",\"lux_rate\":" + std::to_string(TaxField(w, kTaxWinLux)) + "}";
}

int SehMenuExec(int menu, int command) {
  __try {
    GameFn<void(__cdecl*)(int, int)>(kVaMenuExec)(menu, command);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehTaxOk() {
  __try {
    GameFn<void(__cdecl*)()>(kVaTaxWindowOk)();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

// Drops a slider's thumb at pos, the message a mouse release sends.
void DragSlider(char* w, int bar_offset, int pos) {
  HWND bar = *reinterpret_cast<HWND*>(w + bar_offset);
  if (!IsWindow(bar)) return;
  SendMessageA(GetParent(bar), WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, pos), reinterpret_cast<LPARAM>(bar));
}

std::string FinishRates(const std::string& response) {
  g_pending = {};
  g_rates.stage = RatesStage::Idle;
  RefreshShadow();
  return response;
}

std::string HandOffRates() {
  g_pending = {};
  g_rates.stage = RatesStage::Idle;
  return std::string("{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"游戏弹出了对话框，先 respond\",\"tax_window\":") +
         TaxWindowJson() + ",\"dialog\":" + DialogJson() + "}";
}

// The window has no Cancel: OK, Enter and Esc all keep what it shows.
void PressTaxOk() {
  g_rates.stage = RatesStage::Closing;
  g_rates.at = GetTickCount();
  SehTaxOk();
}

// Empty while the command goes on; PollRates finishes it.
std::string BeginRates(const ActArgs& a) {
  if (!HumansTurn()) return ErrJson("not_your_turn", "当前不是人类玩家的行动阶段");
  if (a.tax < 0 || a.science < 0 || a.tax > 10 || a.science > 10 || a.tax + a.science > 10)
    return ErrJson("illegal", "税收和科学比例各 0 到 10，合计不超过 10");
  g_pending = {};
  g_pending.kind = PendingKind::Rates;
  g_pending.posted = GetTickCount();
  g_rates = {};
  g_rates.run = ++g_rates_runs;
  g_rates.want[0] = a.tax;
  g_rates.want[1] = a.science;
  g_rates.want[2] = 10 - a.tax - a.science;
  g_rates.stage = RatesStage::Opening;
  g_rates.at = GetTickCount();
  if (TaxWindow()) return {};  // the game opened it (government change): drive it in its own loop
  // Kingdom > Tax Rate. ShowTaxRate runs the window's loop, where PollRates
  // moves the sliders, and returns once OK has closed it.
  int run = g_rates.run;
  if (SehMenuExec(kMenuKingdom, kMenuTaxRate)) return FinishRates(ErrJson("game_fault", "打开税率窗口时出错"));
  if (g_pending.kind != PendingKind::Rates || g_rates.run != run) return {};
  if (g_rates.stage == RatesStage::Opening) return FinishRates(ErrJson("game_fault", "税率窗口没有打开"));
  return {};
}

std::string PollRates() {
  DWORD now = GetTickCount();
  if (DialogPtr()) return HandOffRates();
  char* w = TaxWindow();
  switch (g_rates.stage) {
    case RatesStage::Opening: {
      if (!w) break;
      if (TaxField(w, kTaxWinCiv) != Human()) return FinishRates(ErrJson("unsupported", "税率窗口不是玩家文明的"));
      g_rates.max_rate = TaxField(w, kTaxWinMaxRate);
      g_rates.start[0] = TaxField(w, kTaxWinTax);
      g_rates.start[1] = TaxField(w, kTaxWinScience);
      g_rates.start[2] = TaxField(w, kTaxWinLux);
      if ((std::max)({g_rates.want[0], g_rates.want[1], g_rates.want[2]}) > g_rates.max_rate) {
        g_rates.refused = "超过了现在政体允许的上限";
        PressTaxOk();
        break;
      }
      g_rates.stage = RatesStage::Sliding;
      g_rates.at = now;
      break;
    }
    case RatesStage::Sliding: {
      if (!w) return FinishRates(ErrJson("game_fault", "税率窗口已经关了"));
      if (now - g_rates.at < kRatesStepMs) break;
      g_rates.at = now;
      const int* target = g_rates.restoring ? g_rates.start : g_rates.want;
      int live[3] = {TaxField(w, kTaxWinTax), TaxField(w, kTaxWinScience), TaxField(w, kTaxWinLux)};
      if (live[0] == target[0] && live[1] == target[1] && live[2] == target[2]) {
        PressTaxOk();
        break;
      }
      if (g_rates.moves >= kRatesMaxMoves) {
        if (g_rates.restoring) {
          PressTaxOk();  // as close as the window lets it get
          break;
        }
        // Locked sliders can make a split unreachable; put the old one back.
        g_rates.refused = "滑块锁定，设不出这组比例";
        g_rates.restoring = true;
        g_rates.moves = 0;
        break;
      }
      // One slider per step, in the order tax, science, lux.
      static const int kBars[3] = {kTaxWinTaxBar, kTaxWinScienceBar, kTaxWinLuxBar};
      for (int k = 0; k < 3; ++k) {
        int slot = (g_rates.moves + k) % 3;
        if (live[slot] == target[slot]) continue;
        DragSlider(w, kBars[slot], target[slot]);
        g_rates.moves += k + 1;
        break;
      }
      break;
    }
    case RatesStage::Closing: {
      // OK only clears the window's flag; the game then commits the rates and
      // recalculates the cities. Read them a moment later.
      if (w || now - g_rates.at < 300) break;
      auto* civ = CivAt(Human());
      std::string body = "\"tax_rate\":" + std::to_string(civ->tax_rate) +
                         ",\"science_rate\":" + std::to_string(civ->science_rate) +
                         ",\"lux_rate\":" + std::to_string(10 - civ->tax_rate - civ->science_rate) +
                         ",\"max_rate\":" + std::to_string(g_rates.max_rate);
      if (!g_rates.refused.empty())
        return FinishRates("{\"ok\":false,\"error\":\"illegal\",\"reason\":" + JsonString(g_rates.refused) + "," + body +
                           "}");
      bool changed = civ->tax_rate != g_rates.start[0] || civ->science_rate != g_rates.start[1];
      return FinishRates(OkJson(body + ",\"changed\":" + (changed ? "true" : "false")));
    }
    default:
      return FinishRates(ErrJson("game_fault", "税率命令状态异常"));
  }
  KeepPumping(now);
  return {};
}

// Opens the city window the way a click on the city does. Empty while the
// command goes on; PollProduce finishes it.
std::string BeginProduce(const ActArgs& a) {
  if (!HumansTurn()) return ErrJson("not_your_turn", "当前不是人类玩家的行动阶段");
  auto* game = G();
  if (a.city < 0 || a.city >= game->total_cities || Cities()[a.city].id == 0) return ErrJson("illegal", "城市不存在");
  if (Cities()[a.city].owner != Human()) return ErrJson("illegal", "这不是己方城市");
  int open = OpenCityWindow();
  if (CityWindowLocked() && open < 0 && !GameModalScreenOpen()) {
    return std::string("{\"ok\":false,\"error\":\"modal_open\",\"reason\":") +
           JsonString("有一扇还没登记的窗口挡着") + ",\"city_window\":null,\"dialog\":" + DialogJson() + "}";
  }
  if (CityWindowLocked() && open != a.city) return ErrJson("illegal", "另一座城市的窗口正等着关闭，先 close-city");
  g_pending = {};
  g_pending.kind = PendingKind::Produce;
  g_pending.posted = GetTickCount();
  g_produce = {};
  g_produce.run = ++g_produce_runs;
  g_produce.city = a.city;
  g_produce.item = a.item;
  g_produce.item_id = a.item_id;
  g_produce.was_open = open == a.city;
  g_produce.at = GetTickCount();
  if (open != a.city) {
    g_produce.stage = ProduceStage::Showing;
    int run = g_produce.run;
    if (SehCityWindowShow(a.city)) return FinishProduce(ErrJson("game_fault", "打开城市窗口时出错"));
    // A popup during Show (tutorial help) may have taken the command over.
    if (g_pending.kind != PendingKind::Produce || g_produce.run != run) return {};
    if (OpenCityWindow() != a.city) return FinishProduce(ErrJson("game_fault", "城市窗口没有打开"));
  }
  g_produce.stage = ProduceStage::Opened;
  g_produce.at = GetTickCount();
  return {};
}

std::string PollProduce() {
  DWORD now = GetTickCount();
  void* dlg = DialogPtr();
  switch (g_produce.stage) {
    case ProduceStage::Showing:
      if (dlg) return HandOffProduce();
      break;
    case ProduceStage::Opened: {
      if (dlg) return HandOffProduce();
      if (now - g_produce.at < kProduceStepMs) break;
      if (OpenCityWindow() != g_produce.city) return FinishProduce(ErrJson("game_fault", "城市窗口已经关了"));
      // Change runs the list's own loop; PollProduce answers it from there
      // (Picking, Picked) and the call returns once the list has closed.
      g_produce.stage = ProduceStage::Picking;
      int run = g_produce.run;
      int fault = SehCityChange();
      if (g_pending.kind != PendingKind::Produce || g_produce.run != run) return {};  // handed off or cancelled
      if (fault) return FinishProduce(ErrJson("game_fault", "「更改」处理函数异常"));
      if (g_produce.stage == ProduceStage::Picking) {
        CloseCityWindowIfOpened();
        return FinishProduce(ErrJson("game_fault", "游戏没有打开生产列表"));
      }
      g_produce.stage = ProduceStage::Changed;
      g_produce.at = now;
      break;
    }
    case ProduceStage::Picking: {
      if (!dlg) break;
      if (!IsProductionList(dlg, g_produce.city)) return HandOffProduce();
      DialogChoices listed = ChoicesFor(dlg);
      std::vector<MenuChoice>& choices = listed.choices;
      int target = -1, cancel = -1, ok = -1;
      for (size_t i = 0; i < choices.size(); ++i) {
        const MenuChoice& c = choices[i];
        if (c.answer == Answer::Button) {
          if (c.std_type == kStdButtonCancel || (cancel < 0 && c.text == "取消")) cancel = static_cast<int>(i);
          if (c.std_type == kStdButtonOk || (ok < 0 && c.text == "确定")) ok = static_cast<int>(i);
          continue;
        }
        bool hit = g_produce.item_id >= 0 ? c.list_id == g_produce.item_id
                                          : !g_produce.item.empty() &&
                                                (c.text == g_produce.item ||
                                                 ProductionName(BuildingOfRecord(c.list_id)) == g_produce.item);
        if (hit && !c.disabled) target = static_cast<int>(i);
      }
      std::string options = ProductionOptionsJson(choices);
      const City& city = Cities()[g_produce.city];
      bool looking = g_produce.item.empty() && g_produce.item_id < 0;
      int pick = -1;
      if (target >= 0 && BuildingOfRecord(choices[target].list_id) != city.building) {
        pick = target;
      } else {
        // Leave the production as it is. This list has no Cancel in some
        // versions; then a player presses OK on the row the game highlighted,
        // which is what the city builds now.
        pick = cancel;
        int highlighted = listed.default_index;
        if (pick < 0 && ok >= 0 && highlighted >= 0 &&
            BuildingOfRecord(choices[highlighted].list_id) == city.building)
          pick = ok;
        std::string head = std::string("\"city\":") + std::to_string(g_produce.city) +
                           ",\"building\":" + std::to_string(city.building) +
                           ",\"building_name\":" + JsonString(ProductionName(city.building));
        if (looking || target >= 0)
          g_produce.answer = OkJson(head + ",\"changed\":false,\"options\":" + options);
        else
          g_produce.answer = std::string("{\"ok\":false,\"error\":\"illegal\",\"reason\":\"生产列表里没有这一项\",") +
                             head + ",\"options\":" + options + "}";
      }
      if (pick < 0) return HandOffProduce();  // no way out of the list a player would use
      std::string answered = AnswerNative(dlg, choices[pick], "\"index\":" + std::to_string(pick));
      if (answered.find("\"error\":\"no_dialog\"") != std::string::npos) break;  // not drawn yet
      if (answered.find("\"ok\":true") == std::string::npos) return HandOffProduce();
      g_produce.list = dlg;
      g_produce.stage = ProduceStage::Picked;
      break;
    }
    case ProduceStage::Picked:
      // The list is closing; a different popup now is the game's confirmation.
      if (dlg && dlg != g_produce.list) return HandOffProduce();
      break;
    case ProduceStage::Changed: {
      if (dlg) return HandOffProduce();
      if (now - g_produce.at < kProduceStepMs) break;
      std::string response = g_produce.answer;
      if (response.empty()) {
        const City& city = Cities()[g_produce.city];
        response = OkJson(std::string("\"city\":") + std::to_string(g_produce.city) +
                          ",\"building\":" + std::to_string(city.building) +
                          ",\"building_name\":" + JsonString(ProductionName(city.building)) +
                          ",\"shields\":" + std::to_string(city.build_progress) + ",\"changed\":true");
      }
      CloseCityWindowIfOpened();
      return FinishProduce(response);
    }
    default:
      return FinishProduce(ErrJson("game_fault", "生产命令状态异常"));
  }
  KeepPumping(now);
  return {};
}

}  // namespace

void GameLog(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  OutputDebugStringA(buf);
  OutputDebugStringA("\n");
  LogFile(buf);
}

void GameInit() {
  g_base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  g_hash = Sha256File(path);
  g_hash_ok = HashAllowed(g_hash);
  g_limits_ok = LimitsAllowed();
  if (!g_hash_ok) g_block = "version_mismatch";
  else if (!g_limits_ok) g_block = "unsupported_limits";
  g_can = reinterpret_cast<FnCanMove>(g_base + (kVaUnitCanMove - kImageBase));
  g_square = reinterpret_cast<FnGetSq>(g_base + (kVaMapGetSquare - kImageBase));
  g_string = reinterpret_cast<FnStr>(g_base + (kVaGetString - kImageBase));
  g_year_fn = reinterpret_cast<FnYear>(g_base + (kVaTurnToYear - kImageBase));
  GameLog("civ2agent init base=%p hash=%s ok=%d limits=%d", g_base, g_hash.c_str(), g_hash_ok,
          g_limits_ok);
}

Gate GameGate() {
  Gate g;
  g.hash_ok = g_hash_ok;
  g.limits_ok = g_limits_ok;
  g.uia_loaded = GetModuleHandleA("Civ2UIA.dll") != nullptr;
  g.exe_hash = g_hash;
  g.block_error = g_block;
  g.in_game = g_hash_ok && g_limits_ok && MatchLive();
  g.multi_type = (g_hash_ok && Readable(G(), sizeof(Game))) ? G()->multi_type : -1;
  return g;
}

bool GameInMatch() { return g_hash_ok && g_limits_ok && MatchLive(); }

bool GameDialogOpen() { return DialogPtr() != nullptr; }

bool GameModalScreenOpen() { return CurrentModal() != ModalKind::None; }

bool GameUnregisteredLock() {
  return MatchLive() && CityWindowLocked() && OpenCityWindow() < 0 && CurrentModal() == ModalKind::None &&
         DialogPtr() == nullptr;
}

void GameObserveDialog() {
  void* now = DialogPtr();
  if (now && now != g_last_dialog) {
    g_last_dialog = now;
    g_dialog_announced = true;
    PushEvent(std::string("{\"type\":\"dialog\",\"dialog\":") + DialogJson() + "}");
  } else if (!now && g_last_dialog) {
    g_last_dialog = nullptr;
    g_dialog_announced = false;
    PushEvent("{\"type\":\"dialog_closed\"}");
  }
  static ModalKind last_screen = ModalKind::None;
  ModalKind screen = now ? ModalKind::None : CurrentModal();
  if (screen != last_screen) {
    if (screen != ModalKind::None && !now)
      PushEvent(std::string("{\"type\":\"dialog\",\"dialog\":") + DialogJson() + "}");
    else if (screen == ModalKind::None && last_screen != ModalKind::None && !now)
      PushEvent("{\"type\":\"dialog_closed\"}");
    last_screen = screen;
  }
  void* notice = NoticePtr();
  if (notice && notice != g_last_notice) PushEvent(std::string("{\"type\":\"notice\",\"notice\":") + NoticeJson() + "}");
  g_last_notice = notice;
}

bool GameTakeModalAbort(std::string& response_json) {
  bool dialog = DialogPtr() != nullptr;
  bool screen = !dialog && GameModalScreenOpen();
  bool tax = !dialog && !screen && GameTaxWindowOpen();
  bool city = !dialog && !screen && !tax && GameCityWindowModal();
  bool unknown = !dialog && !screen && !tax && !city && GameUnregisteredLock();
  if (!dialog && !screen && !tax && !city && !unknown) return false;
  std::string turn;
  if (MatchLive()) {
    turn = std::string("\"turn\":") + std::to_string(G()->turn) + ",\"year\":" + std::to_string(GameYear()) + ",";
    if (g_turn_inflight) {
      bool ended = G()->turn != g_inflight_turn || G()->year != g_inflight_year;
      turn += std::string("\"turn_ended\":") + (ended ? "true" : "false") + ",";
      if (ended) g_turn_inflight = false;
    }
  }
  const char* reason = dialog ? "出现对话框"
                       : screen ? "画面上有一页窗口，用 respond 关闭"
                       : tax    ? "税率窗口打开着，游戏等它关上；用 rates 设好比例并确定"
                       : city   ? "城市窗口打开着，游戏等它关上；用 produce 或 close-city"
                                : "有一扇还没登记的窗口挡着";
  response_json = std::string("{\"ok\":false,\"error\":\"modal_open\",\"reason\":") + JsonString(reason) + "," + turn +
                  "\"city_window\":" + CityWindowJson() + ",\"tax_window\":" + TaxWindowJson() +
                  ",\"dialog\":" + DialogJson() + "}";
  return true;
}

namespace {

// What the game thread last saw, for the pipe thread while a Windows dialog
// holds the game thread.
std::mutex g_gate_mu;
Gate g_gate_cache;
bool g_gate_cached = false;

std::string HelloJson(const Gate& g, bool dialog_open, const std::string& city, const std::string& tax) {
  std::string body = std::string("\"protocol\":") + std::to_string(kProtocolVersion) +
                     ",\"exe_hash\":" + JsonString(g.exe_hash) +
                     ",\"uia_loaded\":" + (g.uia_loaded ? "true" : "false") +
                     ",\"in_game\":" + (g.in_game ? "true" : "false") +
                     ",\"limits_ok\":" + (g.limits_ok ? "true" : "false") +
                     ",\"multi_type\":" + std::to_string(g.multi_type) +
                     ",\"dialog_open\":" + (dialog_open ? "true" : "false") + ",\"city_window\":" + city +
                     ",\"tax_window\":" + tax;
  if (!g.block_error.empty()) {
    return std::string("{\"ok\":false,\"error\":") + JsonString(g.block_error) + "," + body +
           ",\"reason\":" +
           JsonString(g.block_error == "version_mismatch" ? "civ2.exe 哈希不在允许列表"
                                                          : "单位上限补丁已打开") +
           "}";
  }
  return OkJson(body);
}

Gate CachedGate() {
  std::lock_guard<std::mutex> lock(g_gate_mu);
  if (g_gate_cached) return g_gate_cache;
  Gate g;  // set once at init, before the pipe thread starts
  g.hash_ok = g_hash_ok;
  g.limits_ok = g_limits_ok;
  g.exe_hash = g_hash;
  g.block_error = g_block;
  return g;
}

}  // namespace

std::string GameHelloJson() {
  Gate g = GameGate();
  return HelloJson(g, g.hash_ok && (DialogPtr() != nullptr || GameModalScreenOpen()), g.in_game ? CityWindowJson() : std::string("null"),
                   g.in_game ? TaxWindowJson() : std::string("null"));
}

void GameCacheGate() {
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (g_gate_cached && now - last < 250) return;
  last = now;
  Gate g = GameGate();
  std::lock_guard<std::mutex> lock(g_gate_mu);
  g_gate_cache = g;
  g_gate_cached = true;
}

std::string GameHelloDuringSysDialog() { return HelloJson(CachedGate(), true, "null", "null"); }

std::string GameDialogDuringSysDialog(const std::string& sys_dialog_json) {
  Gate g = CachedGate();
  return OkJson(std::string("\"in_game\":") + (g.in_game ? "true" : "false") +
                ",\"city_window\":null,\"tax_window\":null,\"notice\":null,\"dialog\":" + sys_dialog_json);
}

// Blocked reads only what GameInit fixed, so the pipe thread may use it.
bool GameBlockedCached(std::string& err) { return Blocked(err); }

// Only what the player can see on screen: their own civ, units and cities in
// full; other civs' units and cities as the map shows them; tiles as the
// player last saw them; relations as the Foreign Minister shows them.
std::string GameSnapshotJson() {
  std::string err;
  if (Blocked(err)) return err;
  if (!MatchLive()) {
    if (DialogPtr()) return DialogStateJson();
    return ErrJson("not_in_game", "当前不在对局中");
  }
  auto* game = G();
  auto* map = MapH();
  int human = Human();
  auto* civ = CivAt(human);
  if (!Readable(civ, sizeof(CivHead))) return ErrJson("game_fault", "文明数据不可读");

  std::string out;
  out.reserve(1 << 18);
  out += "\"in_game\":true,\"turn\":" + std::to_string(game->turn);
  out += ",\"year\":" + std::to_string(GameYear());
  out += ",\"human\":" + std::to_string(human);
  out += ",\"difficulty\":" + std::to_string(game->difficulty);
  out += ",\"active_unit\":" + std::to_string(game->active_unit);
  out += ",\"move_unit\":" + std::to_string(MoveUnit());
  out += ",\"civ\":{\"name\":" + JsonString(CivText(kVaCivName, human));
  out += ",\"leader\":" + JsonString(CivText(kVaCivLeaderName, human));
  out += ",\"title\":" + JsonString(CivText(kVaCivLeaderTitle, human));
  out += ",\"gold\":" + std::to_string(civ->gold);
  out += ",\"tax_rate\":" + std::to_string(civ->tax_rate);
  out += ",\"science_rate\":" + std::to_string(civ->science_rate);
  out += ",\"lux_rate\":" + std::to_string(10 - civ->tax_rate - civ->science_rate);
  out += ",\"government\":" + std::to_string(civ->government);
  out += ",\"research\":" + std::to_string(civ->researching);
  out += ",\"research_name\":" + JsonString(TechName(civ->researching));
  out += ",\"beakers\":" + std::to_string(civ->beakers);
  out += ",\"research_cost\":" + std::to_string(AdvanceCost(human));
  out += ",\"future_techs\":" + std::to_string(civ->future_techs);
  out += ",\"techs\":[";
  bool first = true;
  for (int i = 0; i < 100; ++i) {
    if (game->techs_discovered[i] & (1u << human)) {
      if (!first) out += ',';
      first = false;
      out += std::to_string(i);
    }
  }
  out += "]}";

  out += ",\"units\":[";
  std::string foreign = ForeignUnitsJson(human);
  if (foreign == "!") return ErrJson("game_fault", "读取地图失败");
  first = true;
  int nu = (std::min)(static_cast<int>(game->total_units), kUnitSlots);
  auto* units = Units();
  auto* types = Types();
  for (int i = 0; i < nu; ++i) {
    auto& u = units[i];
    if (u.id == 0 || u.civ != human) continue;
    int role = -1, att = 0, def = 0;
    std::string uname;
    if (u.type < 62 && Readable(types + u.type, sizeof(UnitType))) {
      role = types[u.type].role;
      att = types[u.type].att;
      def = types[u.type].def;
      uname = ReadGameText(SehString(types[u.type].string_index), 40);
    }
    std::string common = ",\"x\":" + std::to_string(u.x) + ",\"y\":" + std::to_string(u.y) +
                         ",\"type\":" + std::to_string(u.type) + ",\"name\":" + JsonString(uname);
    if (!first) out += ',';
    first = false;
    out += std::string("{\"index\":") + std::to_string(i) + ",\"id\":" + std::to_string(u.id) + common +
           ",\"role\":" + std::to_string(role) + ",\"moves_left\":" + std::to_string(MovesLeft(i)) +
           ",\"orders\":" + std::to_string(u.orders) + ",\"hp_lost\":" + std::to_string(u.hp_lost) +
           ",\"veteran\":" + ((u.attributes & kUnitVeteran) ? "true" : "false") + ",\"att\":" + std::to_string(att) +
           ",\"def\":" + std::to_string(def) + ",\"home\":" + std::to_string(u.home_city) + "}";
  }
  out += "],\"foreign_units\":[" + foreign;
  out += "],\"cities\":[";
  foreign.clear();
  first = true;
  int nc = (std::min)(static_cast<int>(game->total_cities), kCitySlots);
  auto* cities = Cities();
  for (int i = 0; i < nc; ++i) {
    auto& c = cities[i];
    if (c.id == 0) continue;
    std::string where = ",\"name\":" + JsonString(CityName(c)) + ",\"x\":" + std::to_string(c.x) +
                        ",\"y\":" + std::to_string(c.y);
    if (c.owner != human) {
      // The name, place and the size the player saw last, as on the map.
      if (!CityKnownTo(c, human)) continue;
      int fault = 0;
      MapSquare* sq = SehSquare(c.x, c.y, &fault);
      if (fault) return ErrJson("game_fault", "读取地图失败");
      if (!sq || !TileExploredBy(*sq, human)) continue;
      if (!foreign.empty()) foreign += ',';
      foreign += "{\"owner\":" + std::to_string(c.owner) + where +
                 ",\"size\":" + std::to_string(c.revealed_size[human]) + "}";
      continue;
    }
    if (!first) out += ',';
    first = false;
    out += std::string("{\"index\":") + std::to_string(i) + ",\"id\":" + std::to_string(c.id) + where +
           ",\"size\":" + std::to_string(c.size) + ",\"building\":" + std::to_string(c.building) +
           ",\"building_name\":" + JsonString(ProductionName(c.building)) + ",\"food\":" + std::to_string(c.food_storage) +
           ",\"shields\":" + std::to_string(c.build_progress) + ",\"improvements\":[";
    for (int b = 0; b < 5; ++b) {
      if (b) out += ',';
      out += std::to_string(c.improvements[b]);
    }
    out += "]}";
  }
  out += "],\"foreign_cities\":[" + foreign;
  out += "],\"tiles\":[";
  first = true;
  for (int y = 0; y < map->size_y; ++y) {
    // Squares sit where x + y is even; MapGetSquare maps the odd ones onto a
    // neighbour, so they would repeat it.
    for (int x = y & 1; x < map->size_x; x += 2) {
      int fault = 0;
      MapSquare* sq = SehSquare(x, y, &fault);
      if (fault) return ErrJson("game_fault", "读取地图失败");
      if (!sq || !TileExploredBy(*sq, human)) continue;
      int features = KnownFeatures(x, y, human);
      if (features < 0) return ErrJson("game_fault", "读取已知地形失败");
      if (!first) out += ',';
      first = false;
      out += std::string("{\"x\":") + std::to_string(x) + ",\"y\":" + std::to_string(y) +
             ",\"t\":" + std::to_string(sq->terrain) + ",\"f\":" + std::to_string(features) + "}";
    }
  }
  out += "],\"diplomacy\":[";
  first = true;
  for (int i = 1; i < kCivSlots; ++i) {
    if (i == human || !(game->active_players & (1u << i))) continue;
    std::int32_t t = civ->treaties[i];
    const char* status = TreatyStatus(t);
    if (!status) continue;
    if (!first) out += ',';
    first = false;
    out += std::string("{\"civ\":") + std::to_string(i) + ",\"status\":" + JsonString(status) +
           ",\"embassy\":" + ((t & kTreatyEmbassy) ? "true" : "false") + "}";
  }
  out += "],\"city_window\":" + CityWindowJson();
  out += ",\"tax_window\":" + TaxWindowJson();
  out += ",\"notice\":" + NoticeJson();
  out += ",\"dialog\":";
  out += DialogJson();
  RefreshShadow();
  return OkJson(out);
}

std::string GameMenuJson() {
  std::string err;
  if (Blocked(err)) return err;
  return DialogStateJson();
}

std::string GameEventsJson() {
  std::string err;
  if (Blocked(err)) return err;
  GameObserveDialog();
  RefreshShadow();
  std::string out = "\"events\":[";
  for (size_t i = 0; i < g_events.size(); ++i) {
    if (i) out += ',';
    out += g_events[i];
  }
  out += "]";
  g_events.clear();
  return OkJson(out);
}

std::string GameActJson(const ActArgs& a) {
  std::string err;
  const std::string& name = a.name;
  const int unit = a.unit;
  const int x = a.x;
  const int y = a.y;
  // Pick an entry of the dialog's options by index or by exact text.
  if (name == "respond") {
    if (Blocked(err)) return err;
    // Windows dialogs are answered from the pipe thread (server.cpp).
    if (a.windows) return ErrJson("no_dialog", "Windows 对话框已经关了，先用 dialog 看一下");
    if (a.has_file) return ErrJson("illegal", "现在打开的是游戏的对话框，file 只用于保存和打开文件的对话框");
    return ClickMenuOption(a.option, a.text, a.has_edit ? &a.edits : nullptr);
  }
  if (!RequireMatch(err)) return err;
  if (DialogPtr() || GameModalScreenOpen() || GameUnregisteredLock()) {
    std::string held;
    if (GameTakeModalAbort(held)) return held;
  }

  // Game menu items, as a click on them: Windows' file dialog (save, load)
  // or the game's own confirmation (quit) comes next.
  if (name == "save_game" || name == "load_game" || name == "quit_game") {
    if (!HumansTurn()) return ErrJson("not_your_turn", "当前不是人类玩家的行动阶段");
    const wchar_t* shortcut = name == "save_game" ? L"Ctrl+S" : name == "load_game" ? L"Ctrl+L" : L"Ctrl+Q";
    MenuCommand cmd = FindCommand(shortcut);
    if (!cmd.id) return ErrJson("illegal", "游戏菜单里没有这一项");
    if (!cmd.enabled) return ErrJson("illegal", "游戏菜单里这一项是灰的");
    HWND main = MainWindow();
    if (!main) return ErrJson("game_fault", "找不到游戏窗口");
    PostMessageA(main, WM_COMMAND, cmd.id, 0);
    return OkJson("\"posted\":true");
  }

  if (name == "activate" || name == "move" || name == "goto" || name == "order" ||
      name == "found_city") {
    if (!OwnUnit(unit, err)) return err;
  }

  if (name == "activate") {
    if (Moving(unit)) return OkJson(UnitBrief(unit) + ",\"changed\":false");
    if (MovesLeft(unit) <= 0) return ErrJson("illegal", "这个单位这回合已经没有移动力");
    const Unit& u = Units()[unit];
    // On a stack the click opens the units-list popup: this returns
    // modal_open with it, and the agent picks the unit by its id.
    if (SehClickUnit(u.x, u.y)) return ErrJson("game_fault", "选中单位时出错");
    RefreshShadow();
    if (!Moving(unit)) return ErrJson("illegal", "点了这个单位也选不中它，它这回合已经不能行动");
    return OkJson(UnitBrief(unit) + ",\"changed\":true");
  }

  if (name == "move" || name == "goto") {
    auto& u = Units()[unit];
    int dir = name == "move" ? DirectionTo(u.x, u.y, x, y) : StepToward(u.x, u.y, x, y);
    if (name == "move" && dir < 0) return ErrJson("illegal", "目标不是相邻格");
    if (name == "goto" && u.x == x && u.y == y) return OkJson(UnitBrief(unit) + ",\"changed\":false");
    if (dir < 0) return ErrJson("illegal", "找不到朝目标走的一步");
    HWND map = MapWindow();
    if (!map) return ErrJson("game_fault", "找不到地图窗口");
    std::string refused = SelectForCommand(unit);
    if (!refused.empty()) return refused;
    int can = 0;
    if (SehCan(unit, &can) || !can) return ErrJson("illegal", "这个单位现在不能移动");
    BeginUnitPending(name == "move" ? PendingKind::Move : PendingKind::Goto, unit, x, y);
    PostKey(map, DirectionKey(dir));
    return {};
  }

  if (name == "order") {
    const std::string& order = a.order;
    const wchar_t* shortcut = OrderShortcut(order);
    if (!shortcut) return ErrJson("illegal", "未知命令 " + order);
    std::string refused = SelectForCommand(unit);
    if (!refused.empty()) return refused;
    MenuCommand cmd = FindCommand(shortcut);
    if (!cmd.id) return ErrJson("illegal", "命令菜单里没有这一项，当前单位不能执行 " + order);
    if (!cmd.enabled) return ErrJson("illegal", "命令菜单里这一项是灰的，当前单位不能执行 " + order);
    BeginUnitPending(PendingKind::Order, unit, 0, 0);
    PostMessageA(MainWindow(), WM_COMMAND, cmd.id, 0);
    return {};
  }

  if (name == "found_city") {
    auto& u = Units()[unit];
    if (u.type >= 62 || Types()[u.type].role != 5) return ErrJson("illegal", "只有拓荒者或工程师能建城");
    std::string refused = SelectForCommand(unit);
    if (!refused.empty()) return refused;
    MenuCommand cmd = FindCommand(L"b");
    if (!cmd.id) return ErrJson("illegal", "命令菜单里没有建立新城市");
    if (!cmd.enabled) return ErrJson("illegal", "建立新城市现在是灰的，这里不能建城");
    BeginUnitPending(PendingKind::FoundCity, unit, 0, 0);
    PostMessageA(MainWindow(), WM_COMMAND, cmd.id, 0);
    return {};
  }

  if (name == "produce") return BeginProduce(a);

  if (name == "close_city") {
    // The city window's Exit button.
    if (OpenCityWindow() < 0) return ErrJson("illegal", "没有打开的城市窗口");
    if (SehCityWindowExit()) return ErrJson("game_fault", "关闭城市窗口时出错");
    return OkJson("");
  }

  if (name == "rates") return BeginRates(a);

  return ErrJson("illegal", "未知动作 " + name);
}

std::string GameBeginEndTurnJson() {
  std::string err;
  if (!RequireMatch(err)) return err;
  if (DialogPtr() || GameModalScreenOpen() || GameUnregisteredLock()) {
    std::string held;
    if (GameTakeModalAbort(held)) return held;
  }
  auto* game = G();
  if (g_turn_inflight && (game->turn != g_inflight_turn || game->year != g_inflight_year)) {
    g_turn_inflight = false;
  }
  if (g_turn_inflight) {
    g_pending = {};
    g_pending.kind = PendingKind::EndTurn;
    g_pending.turn = g_inflight_turn;
    g_pending.year = g_inflight_year;
    g_pending.posted = GetTickCount();
    return {};
  }
  if (!PostEndTurn()) return ErrJson("game_fault", "找不到游戏窗口");
  g_turn_inflight = true;
  g_inflight_turn = game->turn;
  g_inflight_year = game->year;
  g_pending = {};
  g_pending.kind = PendingKind::EndTurn;
  g_pending.turn = game->turn;
  g_pending.year = game->year;
  g_pending.posted = GetTickCount();
  return {};
}

std::string GameBeginSkipIntroJson() {
  std::string err;
  if (Blocked(err)) return err;
  if (MatchLive()) return SkipIntroJson(false);
  if (MenuHasStartGame()) return SkipIntroJson(false);
  g_pending = {};
  g_pending.kind = PendingKind::SkipIntro;
  g_pending.posted = GetTickCount();
  EnumWindows(KickWindow, kKickEscape);
  return {};
}

bool GamePendingActive() { return g_pending.kind != PendingKind::None; }

std::string GamePollPending(int depth) {
  if (g_pending.kind == PendingKind::None) return {};
  // Step through the city window, the tax window and their own loops, at any depth.
  if (g_pending.kind == PendingKind::Produce) return PollProduce();
  if (g_pending.kind == PendingKind::Rates) return PollRates();
  DWORD now = GetTickCount();
  if (g_pending.kind == PendingKind::SkipIntro) {
    if (MatchLive() || MenuHasStartGame()) {
      g_pending = {};
      return SkipIntroJson(true);
    }
    if (now - g_pending.posted > 400) {
      EnumWindows(KickWindow, kKickEscape);
      g_pending.posted = now;
    }
    KeepPumping(now);
    return {};
  }
  auto* game = G();
  if (g_pending.kind == PendingKind::EndTurn && !DialogPtr() && !GameModalScreenOpen() && !GameUnregisteredLock()) {
    bool advanced = game->turn != g_pending.turn || game->year != g_pending.year;
    if (advanced && depth == 1) {
      g_pending = {};
      g_turn_inflight = false;
      return TurnJson();
    }
    // After 结束全部回合 the game can stop on its end-of-turn prompt, a nested
    // loop that waits for Enter on the map.
    if (!advanced && now - g_pending.posted > kRepostEndTurnMs) {
      if (depth == 1) {
        PostEndTurn();
      } else if (HWND map = MapWindow()) {
        PostKey(map, VK_RETURN);
      }
      g_pending.posted = now;
    }
    KeepPumping(now);
    return {};
  }
  // The game only returns to its outermost message loop once it is waiting
  // for the player again, so results are read there.
  if (depth != 1 || DialogPtr()) {
    KeepPumping(now);
    return {};
  }
  if (!UnitChanged()) {
    if (now - g_pending.posted < kNoEffectMs) {
      KeepPumping(now);
      return {};
    }
    int unit = g_pending.unit;
    g_pending = {};
    RefreshShadow();
    return OkJson(UnitBrief(unit) + ",\"changed\":false");
  }
  int unit = g_pending.unit;
  if (g_pending.kind == PendingKind::FoundCity) {
    int before = g_pending.cities;
    g_pending = {};
    RefreshShadow();
    return OkJson(std::string("\"cities_before\":") + std::to_string(before) +
                  ",\"cities_after\":" + std::to_string(game->total_cities) + ",\"changed\":true");
  }
  if (g_pending.kind == PendingKind::Goto && UnitAlive() && game->active_unit == unit) {
    auto& u = Units()[unit];
    bool moved = u.x != g_pending.x || u.y != g_pending.y;
    int can = 0;
    if (moved && !(u.x == g_pending.target_x && u.y == g_pending.target_y) && !SehCan(unit, &can) && can) {
      int dir = StepToward(u.x, u.y, g_pending.target_x, g_pending.target_y);
      HWND map = MapWindow();
      if (dir >= 0 && map) {
        SnapshotUnit();
        PostKey(map, DirectionKey(dir));
        KeepPumping(now);
        return {};
      }
    }
  }
  g_pending = {};
  RefreshShadow();
  if (unit < 0 || unit >= kUnitSlots || Units()[unit].id == 0) {
    return OkJson(std::string("\"index\":") + std::to_string(unit) + ",\"lost\":true,\"changed\":true");
  }
  return OkJson(UnitBrief(unit) + ",\"changed\":true");
}

void GameCancelPending() {
  g_pending = {};
  g_produce.stage = ProduceStage::Idle;
  g_rates.stage = RatesStage::Idle;
}

bool GameTaxWindowOpen() { return g_hash_ok && MatchLive() && TaxWindow() != nullptr; }

bool GameWindowModal() { return GameCityWindowModal() || GameTaxWindowOpen(); }

bool GameCityWindowOpen() { return g_hash_ok && MatchLive() && OpenCityWindow() >= 0; }

bool GameCityWindowModal() { return GameCityWindowOpen() && CityWindowLocked(); }

// Civ2UIA's wait (MsgWaitForMultipleObjectsEx, QS_ALLINPUT, at most a few
// ms) wakes on any message, so the caller's WM_NULL is enough everywhere.
// Clicks and keys go only to the opening movie: posted to every control they
// pressed buttons in whatever window was open (the tax window's OK).
void GameWakeUi(bool press_escape) {
  static DWORD last_log = 0;
  DWORD now = GetTickCount();
  if (now - last_log > 2000) {
    last_log = now;
    GameLog("wake ui escape=%d pending=%d turn=%d", press_escape ? 1 : 0, static_cast<int>(g_pending.kind),
            MatchLive() ? G()->turn : -1);
  }
  if (press_escape) EnumWindows(KickWindow, kKickEscape);
}
