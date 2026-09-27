#include "game.h"
#include "jsonutil.h"
#include "types.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::uint8_t* g_base = nullptr;
std::string g_hash;
std::string g_block;
bool g_hash_ok = false;
bool g_limits_ok = false;
void* g_last_dialog = nullptr;
bool g_dialog_announced = false;

struct ShadowUnit {
  int id = 0;
  int hp = 0;
  int alive = 0;
};
struct ShadowCity {
  int id = 0;
  int owner = -1;
};
ShadowUnit g_units[kUnitSlots];
ShadowCity g_cities[kCitySlots];
bool g_shadow_ready = false;
std::vector<std::string> g_events;

using FnAfter = void(__cdecl*)(int);
using FnCanMove = int(__cdecl*)(int);
using FnGetSq = MapSquare*(__cdecl*)(int, int);
using FnVisible = int(__cdecl*)(int, int, int);
using FnCalc = int(__cdecl*)(int, int);
using FnHasTech = int(__cdecl*)(int, int);
using FnStr = char*(__cdecl*)(int);
using FnYear = int(__cdecl*)(int);

FnAfter g_after = nullptr;
FnCanMove g_can = nullptr;
FnGetSq g_square = nullptr;
FnVisible g_visible = nullptr;
FnCalc g_calc = nullptr;
FnHasTech g_has_tech = nullptr;
FnStr g_string = nullptr;
FnYear g_year_fn = nullptr;

enum class PendingKind { None, EndTurn, FoundCity, Move, Goto, Order, SkipIntro };

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

int SehAfter(int v) {
  __try {
    g_after(v);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehCan(int unit, int* ret) {
  __try {
    *ret = g_can(unit);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehCalc(int city) {
  __try {
    g_calc(city, 1);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

int SehHas(int civ, int tech, int* ret) {
  __try {
    *ret = g_has_tech(civ, tech);
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

void* DialogPtr() {
  auto** slot = At<void*>(kVaPopup);
  if (!Readable(slot, sizeof(void*))) return nullptr;
  void* dlg = *slot;
  if (!Readable(dlg, 0x2F4)) return nullptr;
  int buttons = *reinterpret_cast<int*>(static_cast<char*>(dlg) + kDlgNumButtons);
  if (buttons < 0 || buttons > 8) return nullptr;
  return dlg;
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

std::string DialogJson() {
  void* dlg = DialogPtr();
  if (!dlg) return "null";
  auto* base = static_cast<char*>(dlg);
  const char* title = *reinterpret_cast<char**>(base + kDlgTitle);
  int buttons = *reinterpret_cast<int*>(base + kDlgNumButtons);
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
  std::string out = "{\"title\":" + JsonString(ReadGameText(title, 120)) + ",\"text\":" +
                    JsonString(body) + ",\"buttons\":[";
  auto** texts = reinterpret_cast<char**>(base + kDlgButtonTexts);
  for (int i = 0; i < buttons && i < 6; ++i) {
    if (i) out += ',';
    const char* t = Readable(texts + i, sizeof(char*)) ? texts[i] : nullptr;
    out += JsonString(ReadGameText(t, 80));
  }
  out += "]}";
  return out;
}

void PushEvent(const std::string& json_object) { g_events.push_back(json_object); }

void RefreshShadow() {
  if (!MatchLive()) {
    g_shadow_ready = false;
    return;
  }
  auto* game = G();
  auto* units = Units();
  auto* cities = Cities();
  int nu = game->total_units;
  int nc = game->total_cities;
  if (g_shadow_ready) {
    for (int i = 0; i < nu && i < kUnitSlots; ++i) {
      if (!g_units[i].alive) continue;
      bool gone = units[i].id == 0 || units[i].id != g_units[i].id;
      if (gone) {
        PushEvent(std::string("{\"type\":\"unit_lost\",\"index\":") + std::to_string(i) +
                  ",\"id\":" + std::to_string(g_units[i].id) + "}");
      } else if (units[i].hp_lost > g_units[i].hp) {
        PushEvent(std::string("{\"type\":\"unit_damaged\",\"index\":") + std::to_string(i) +
                  ",\"id\":" + std::to_string(units[i].id) +
                  ",\"hp_lost\":" + std::to_string(units[i].hp_lost) + "}");
      }
    }
    for (int i = 0; i < nc && i < kCitySlots; ++i) {
      if (g_cities[i].id && cities[i].id == g_cities[i].id && cities[i].owner != g_cities[i].owner) {
        PushEvent(std::string("{\"type\":\"city_captured\",\"index\":") + std::to_string(i) +
                  ",\"id\":" + std::to_string(cities[i].id) +
                  ",\"owner\":" + std::to_string(cities[i].owner) + "}");
      }
    }
  }
  for (int i = 0; i < kUnitSlots; ++i) {
    if (i < nu && units[i].id != 0) {
      g_units[i].alive = 1;
      g_units[i].id = units[i].id;
      g_units[i].hp = units[i].hp_lost;
    } else {
      g_units[i] = {};
    }
  }
  for (int i = 0; i < kCitySlots; ++i) {
    if (i < nc && cities[i].id != 0) {
      g_cities[i].id = cities[i].id;
      g_cities[i].owner = cities[i].owner;
    } else {
      g_cities[i] = {};
    }
  }
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

void Activate(int index) {
  G()->active_unit = static_cast<std::int16_t>(index);
  auto* selected = At<int>(kVaUnitSelected);
  if (Readable(selected, sizeof(int))) *selected = 0;
  SehAfter(0);
}

std::string UnitBrief(int index) {
  auto& u = Units()[index];
  return std::string("\"index\":") + std::to_string(index) + ",\"x\":" + std::to_string(u.x) +
         ",\"y\":" + std::to_string(u.y) + ",\"orders\":" + std::to_string(u.orders) +
         ",\"move\":" + std::to_string(u.move_points);
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
  if (game->some_civ >= 0 && game->some_civ <= 7 && game->some_civ != Human()) {
    err = ErrJson("not_your_turn", "当前不是人类玩家的行动阶段");
    return false;
  }
  return true;
}

void NoteResearchChange(int before, int after, int future_before, int future_after) {
  if (before != after || future_before != future_after) {
    PushEvent(std::string("{\"type\":\"research\",\"tech\":") + std::to_string(after) +
              ",\"future\":" + std::to_string(future_after) + "}");
  }
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

struct MenuChoice {
  std::string text;
  HWND hwnd = nullptr;
  int list_index = -1;
  int list_id = -1;
  bool line = false;
};

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
    std::string label = ReadGameText(text, 80);
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

std::string MenuJsonObject() {
  void* dlg = DialogPtr();
  if (!dlg) return ErrJson("no_dialog", "没有打开的界面");
  std::string title;
  std::vector<std::string> ignored;
  CollectMenuLabels(dlg, title, ignored);
  bool is_list = false;
  std::vector<MenuChoice> choices = BuildMenuChoices(dlg, &is_list);
  int selected = -1;
  if (is_list) ReadDialogList(dlg, &selected);
  std::string out = std::string("\"in_game\":") + (MatchLive() ? "true" : "false") + ",\"menu\":{\"title\":" +
                    JsonString(title) + ",\"kind\":" + JsonString(is_list ? "list" : "controls");
  if (is_list) out += ",\"selected\":" + std::to_string(selected);
  out += ",\"options\":[";
  for (size_t i = 0; i < choices.size(); ++i) {
    if (i) out += ',';
    out += std::string("{\"index\":") + std::to_string(i) + ",\"text\":" + JsonString(choices[i].text);
    if (choices[i].list_id >= 0) out += ",\"id\":" + std::to_string(choices[i].list_id);
    out += "}";
  }
  out += "]}";
  return OkJson(out);
}

// List rows are drawn by the dialog itself, so they are picked with the arrow
// keys and Enter. Buttons and menu rows are real controls that ignore posted
// mouse messages; for those the caller gets screen coordinates and clicks
// with the real cursor.
std::string ClickMenuOption(int index, const std::string& text) {
  void* dlg = DialogPtr();
  if (!dlg) return ErrJson("no_dialog", "没有打开的界面");
  std::vector<MenuChoice> choices = BuildMenuChoices(dlg, nullptr);
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
  if (choice.list_index >= 0) {
    HWND hwnd = DialogWindow(dlg);
    if (!hwnd) return ErrJson("no_dialog", "找不到对话框窗口");
    int selected = -1;
    ReadDialogList(dlg, &selected);
    if (selected < 0) selected = 0;
    int delta = choice.list_index - selected;
    for (int i = 0; i < delta; ++i) PostKey(hwnd, VK_DOWN);
    for (int i = 0; i < -delta; ++i) PostKey(hwnd, VK_UP);
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
  std::vector<MenuChoice> choices = BuildMenuChoices(dlg, nullptr);
  for (const auto& choice : choices) {
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

std::string TurnJson() {
  auto* game = G();
  RefreshShadow();
  auto* civ = CivAt(Human());
  int year_text = game->year;
  if (SehYear(game->turn, &year_text)) year_text = game->year;
  return OkJson(std::string("\"turn\":") + std::to_string(game->turn) + ",\"year\":" +
                std::to_string(game->year) + ",\"year_computed\":" + std::to_string(year_text) +
                ",\"gold\":" + std::to_string(civ->gold));
}

BOOL CALLBACK InvalidateChild(HWND hwnd, LPARAM) {
  InvalidateRect(hwnd, nullptr, FALSE);
  return TRUE;
}

void RepaintPanels() {
  HWND main = MainWindow();
  if (!main) return;
  EnumChildWindows(main, InvalidateChild, 0);
  RedrawWindow(main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
  g_after = reinterpret_cast<FnAfter>(g_base + (kVaAfterActive - kImageBase));
  g_can = reinterpret_cast<FnCanMove>(g_base + (kVaUnitCanMove - kImageBase));
  g_square = reinterpret_cast<FnGetSq>(g_base + (kVaMapGetSquare - kImageBase));
  g_visible = reinterpret_cast<FnVisible>(g_base + (kVaMapVisible - kImageBase));
  g_calc = reinterpret_cast<FnCalc>(g_base + (kVaCalcCity - kImageBase));
  g_has_tech = reinterpret_cast<FnHasTech>(g_base + (kVaCivHasTech - kImageBase));
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
}

bool GameTakeModalAbort(std::string& response_json) {
  if (!DialogPtr()) return false;
  std::string turn;
  if (MatchLive()) {
    turn = std::string("\"turn\":") + std::to_string(G()->turn) + ",\"year\":" + std::to_string(G()->year) + ",";
    if (g_turn_inflight) {
      bool ended = G()->turn != g_inflight_turn || G()->year != g_inflight_year;
      turn += std::string("\"turn_ended\":") + (ended ? "true" : "false") + ",";
      if (ended) g_turn_inflight = false;
    }
  }
  response_json = std::string("{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"出现对话框\",") + turn +
                  "\"dialog\":" + DialogJson() + "}";
  return true;
}

std::string GameHelloJson() {
  Gate g = GameGate();
  std::string body = std::string("\"protocol\":") + std::to_string(kProtocolVersion) +
                     ",\"exe_hash\":" + JsonString(g.exe_hash) +
                     ",\"uia_loaded\":" + (g.uia_loaded ? "true" : "false") +
                     ",\"in_game\":" + (g.in_game ? "true" : "false") +
                     ",\"limits_ok\":" + (g.limits_ok ? "true" : "false") +
                     ",\"multi_type\":" + std::to_string(g.multi_type);
  if (!g.block_error.empty()) {
    return std::string("{\"ok\":false,\"error\":") + JsonString(g.block_error) + "," + body +
           ",\"reason\":" +
           JsonString(g.block_error == "version_mismatch" ? "civ2.exe 哈希不在允许列表"
                                                          : "单位上限补丁已打开") +
           "}";
  }
  return OkJson(body);
}

std::string GameSnapshotJson(bool debug) {
  std::string err;
  if (Blocked(err)) return err;
  if (!MatchLive()) {
    if (DialogPtr()) return MenuJsonObject();
    return ErrJson("not_in_game", "当前不在对局中");
  }
  auto* game = G();
  auto* map = MapH();
  int human = Human();
  auto* civ = CivAt(human);
  if (!Readable(civ, sizeof(CivHead))) return ErrJson("game_fault", "文明数据不可读");
  int year_text = game->year;
  int yfault = SehYear(game->turn, &year_text);
  if (yfault) year_text = game->year;

  std::string research_name;
  if (civ->researching >= 0 && civ->researching < 100) {
    auto* rule = Rules() + civ->researching;
    if (Readable(rule, sizeof(RulesCivilize))) {
      const char* s = SehString(rule->text_index);
      research_name = ReadGameText(s, 80);
    }
  }

  std::string out;
  out.reserve(1 << 16);
  out += "\"turn\":" + std::to_string(game->turn);
  out += ",\"year\":" + std::to_string(game->year);
  out += ",\"year_computed\":" + std::to_string(year_text);
  out += ",\"human\":" + std::to_string(human);
  out += ",\"multi_type\":" + std::to_string(game->multi_type);
  out += ",\"gold\":" + std::to_string(civ->gold);
  out += ",\"tax_rate\":" + std::to_string(civ->tax_rate);
  out += ",\"science_rate\":" + std::to_string(civ->science_rate);
  out += ",\"government\":" + std::to_string(civ->government);
  out += ",\"research\":" + std::to_string(civ->researching);
  out += ",\"research_name\":" + JsonString(research_name);
  out += ",\"future_techs\":" + std::to_string(civ->future_techs);
  out += ",\"active_unit\":" + std::to_string(game->active_unit);
  out += ",\"difficulty\":" + std::to_string(game->difficulty);
  out += ",\"techs\":[";
  bool first = true;
  for (int i = 0; i < 100; ++i) {
    if (game->techs_discovered[i] & (1u << human)) {
      if (!first) out += ',';
      first = false;
      out += std::to_string(i);
    }
  }
  out += "],\"treaties\":[";
  for (int i = 0; i < 8; ++i) {
    if (i) out += ',';
    out += std::to_string(civ->treaties[i]);
  }
  out += "]";

  out += ",\"units\":[";
  first = true;
  int nu = game->total_units;
  auto* units = Units();
  auto* types = Types();
  for (int i = 0; i < nu; ++i) {
    auto& u = units[i];
    if (u.id == 0) continue;
    bool own = u.civ == human;
    bool see = own || debug;
    if (!see) {
      int fault = 0;
      MapSquare* sq = SehSquare(u.x, u.y, &fault);
      if (fault) return ErrJson("game_fault", "读取地图失败");
      if (sq && (sq->visibility & (1u << human))) see = true;
    }
    if (!see) continue;
    if (!first) out += ',';
    first = false;
    int role = -1, att = 0, def = 0;
    std::string uname;
    if (u.type < 62 && Readable(types + u.type, sizeof(UnitType))) {
      role = types[u.type].role;
      att = types[u.type].att;
      def = types[u.type].def;
      const char* s = SehString(types[u.type].string_index);
      uname = ReadGameText(s, 40);
    }
    out += std::string("{\"index\":") + std::to_string(i) + ",\"id\":" + std::to_string(u.id) +
           ",\"x\":" + std::to_string(u.x) + ",\"y\":" + std::to_string(u.y) +
           ",\"type\":" + std::to_string(u.type) + ",\"name\":" + JsonString(uname) +
           ",\"role\":" + std::to_string(role) + ",\"civ\":" + std::to_string(u.civ) +
           ",\"move\":" + std::to_string(u.move_points) + ",\"orders\":" + std::to_string(u.orders) +
           ",\"hp_lost\":" + std::to_string(u.hp_lost) + ",\"att\":" + std::to_string(att) +
           ",\"def\":" + std::to_string(def) + ",\"home\":" + std::to_string(u.home_city) + "}";
  }
  out += "],\"cities\":[";
  first = true;
  int nc = game->total_cities;
  auto* cities = Cities();
  for (int i = 0; i < nc; ++i) {
    auto& c = cities[i];
    if (c.id == 0) continue;
    bool own = c.owner == human;
    bool see = own || debug;
    if (!see) {
      int fault = 0;
      MapSquare* sq = SehSquare(c.x, c.y, &fault);
      if (fault) return ErrJson("game_fault", "读取地图失败");
      if (sq && (sq->visibility & (1u << human))) see = true;
    }
    if (!see) continue;
    if (!first) out += ',';
    first = false;
    char namebuf[17] = {};
    memcpy(namebuf, c.name, 16);
    out += std::string("{\"index\":") + std::to_string(i) + ",\"id\":" + std::to_string(c.id) +
           ",\"name\":" + JsonString(ReadGameText(namebuf, 16)) + ",\"x\":" + std::to_string(c.x) +
           ",\"y\":" + std::to_string(c.y) + ",\"owner\":" + std::to_string(c.owner) +
           ",\"size\":" + std::to_string(c.size) + ",\"building\":" + std::to_string(c.building) +
           ",\"food\":" + std::to_string(c.food_storage) +
           ",\"shields\":" + std::to_string(c.build_progress) + ",\"improvements\":[";
    for (int b = 0; b < 5; ++b) {
      if (b) out += ',';
      out += std::to_string(c.improvements[b]);
    }
    out += "]}";
  }
  out += "],\"tiles\":[";
  first = true;
  int tiles = 0;
  for (int y = 0; y < map->size_y && tiles < 20000; ++y) {
    for (int x = 0; x < map->size_x && tiles < 20000; ++x) {
      int fault = 0;
      MapSquare* sq = SehSquare(x, y, &fault);
      if (fault) return ErrJson("game_fault", "读取地图失败");
      if (!sq) continue;
      bool see = debug || (sq->visibility & (1u << human));
      if (!see) continue;
      if (!first) out += ',';
      first = false;
      ++tiles;
      out += std::string("{\"x\":") + std::to_string(x) + ",\"y\":" + std::to_string(y) +
             ",\"t\":" + std::to_string(sq->terrain) + ",\"f\":" + std::to_string(sq->features) +
             ",\"v\":" + std::to_string(sq->visibility) + "}";
    }
  }
  out += "]";
  if (debug) {
    out += ",\"civs\":[";
    for (int i = 0; i < 8; ++i) {
      if (i) out += ',';
      auto* c = CivAt(i);
      out += std::string("{\"index\":") + std::to_string(i) + ",\"gold\":" + std::to_string(c->gold) +
             ",\"research\":" + std::to_string(c->researching) +
             ",\"government\":" + std::to_string(c->government) + "}";
    }
    out += "]";
  }
  out += ",\"dialog\":";
  out += DialogJson();
  RefreshShadow();
  return OkJson(out);
}

std::string GameMenuJson() {
  std::string err;
  if (Blocked(err)) return err;
  return MenuJsonObject();
}

std::string GameEventsJson() {
  std::string err;
  if (Blocked(err)) return err;
  GameObserveDialog();
  std::string out = "\"events\":[";
  for (size_t i = 0; i < g_events.size(); ++i) {
    if (i) out += ',';
    out += g_events[i];
  }
  out += "]";
  g_events.clear();
  return OkJson(out);
}

std::string GameActJson(const std::string& name, int unit, int city, int x, int y, int id, int tax,
                        int science, int button, const std::string& kind, const std::string& order) {
  std::string err;
  if (name == "menu" || name == "respond") {
    if (Blocked(err)) return err;
    if (name == "respond" && DialogPtr()) {
      int buttons = *reinterpret_cast<int*>(static_cast<char*>(DialogPtr()) + kDlgNumButtons);
      if (buttons > 0 && order.empty()) return ClickMenuOption(button, "");
    }
    if (name == "menu" || (name == "respond" && DialogPtr() &&
                           *reinterpret_cast<int*>(static_cast<char*>(DialogPtr()) + kDlgNumButtons) <= 0)) {
      return ClickMenuOption(button, order);
    }
  }
  if (!RequireMatch(err)) return err;
  if (DialogPtr() && name != "respond") {
    return std::string("{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"已有对话框\",") +
           "\"dialog\":" + DialogJson() + "}";
  }
  auto* civ = CivAt(Human());
  int research_before = civ->researching;
  int future_before = civ->future_techs;

  if (name == "respond") {
    void* dlg = DialogPtr();
    if (!dlg) return ErrJson("no_dialog", "没有打开的对话框");
    auto* base = static_cast<char*>(dlg);
    int buttons = *reinterpret_cast<int*>(base + kDlgNumButtons);
    if (button < 0 || button >= buttons) return ErrJson("illegal", "按钮下标超出范围");
    auto* controls = *reinterpret_cast<char**>(base + kDlgButtonControls);
    int code = button;
    HWND hwnd = nullptr;
    if (controls && Readable(controls, kButtonStride * (button + 1))) {
      char* btn = controls + button * kButtonStride;
      code = *reinterpret_cast<int*>(btn + kButtonCode);
      hwnd = *reinterpret_cast<HWND*>(btn + kButtonHwnd);
    }
    *reinterpret_cast<int*>(base + kDlgPressed) = code;
    if (hwnd && IsWindow(hwnd)) PostMessageA(hwnd, BM_CLICK, 0, 0);
    HWND top = GetForegroundWindow();
    if (top) PostMessageA(top, WM_NULL, 0, 0);
    RefreshShadow();
    return OkJson(std::string("\"button\":") + std::to_string(button) + ",\"code\":" +
                  std::to_string(code));
  }

  if (name == "activate" || name == "move" || name == "goto" || name == "order" ||
      name == "found_city") {
    if (!OwnUnit(unit, err)) return err;
  }

  if (name == "activate") {
    Activate(unit);
    RefreshShadow();
    return OkJson(UnitBrief(unit));
  }

  if (name == "move" || name == "goto") {
    auto& u = Units()[unit];
    int dir = name == "move" ? DirectionTo(u.x, u.y, x, y) : StepToward(u.x, u.y, x, y);
    if (name == "move" && dir < 0) return ErrJson("illegal", "目标不是相邻格");
    if (name == "goto" && u.x == x && u.y == y) return OkJson(UnitBrief(unit) + ",\"changed\":false");
    if (dir < 0) return ErrJson("illegal", "找不到朝目标走的一步");
    int can = 0;
    if (SehCan(unit, &can) || !can) return ErrJson("illegal", "这个单位现在不能移动");
    HWND map = MapWindow();
    if (!map) return ErrJson("game_fault", "找不到地图窗口");
    if (G()->active_unit != unit) Activate(unit);
    BeginUnitPending(name == "move" ? PendingKind::Move : PendingKind::Goto, unit, x, y);
    PostKey(map, DirectionKey(dir));
    return {};
  }

  if (name == "order") {
    const wchar_t* shortcut = OrderShortcut(order);
    if (!shortcut) return ErrJson("illegal", "未知命令 " + order);
    if (G()->active_unit != unit) Activate(unit);
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
    if (G()->active_unit != unit) Activate(unit);
    MenuCommand cmd = FindCommand(L"b");
    if (!cmd.id) return ErrJson("illegal", "命令菜单里没有建立新城市");
    if (!cmd.enabled) return ErrJson("illegal", "建立新城市现在是灰的，这里不能建城");
    BeginUnitPending(PendingKind::FoundCity, unit, 0, 0);
    PostMessageA(MainWindow(), WM_COMMAND, cmd.id, 0);
    return {};
  }

  if (name == "produce") {
    auto* game = G();
    if (city < 0 || city >= game->total_cities || Cities()[city].id == 0)
      return ErrJson("illegal", "城市不存在");
    if (Cities()[city].owner != Human()) return ErrJson("illegal", "这不是己方城市");
    if (kind == "unit") {
      if (id < 0 || id > 61) return ErrJson("illegal", "单位类型无效");
      Cities()[city].building = static_cast<std::int8_t>(id);
    } else if (kind == "improvement") {
      if (id <= 0 || id > 66) return ErrJson("illegal", "建筑编号无效");
      Cities()[city].building = static_cast<std::int8_t>(-id);
    } else {
      return ErrJson("illegal", "kind 应为 unit 或 improvement");
    }
    if (SehCalc(city)) return ErrJson("game_fault", "CalcCityGlobals 调用异常");
    RefreshShadow();
    return OkJson(std::string("\"city\":") + std::to_string(city) +
                  ",\"building\":" + std::to_string(Cities()[city].building));
  }

  if (name == "research") {
    if (id < -1 || id > 99) return ErrJson("illegal", "科技编号无效");
    if (id >= 0) {
      int has = 0;
      if (SehHas(Human(), id, &has)) return ErrJson("game_fault", "CivHasTech 调用异常");
      if (has) return ErrJson("illegal", "已经拥有这项科技");
    }
    civ->researching = static_cast<std::int16_t>(id);
    NoteResearchChange(research_before, civ->researching, future_before, civ->future_techs);
    RefreshShadow();
    return OkJson(std::string("\"research\":") + std::to_string(civ->researching));
  }

  if (name == "rates") {
    if (tax < 0 || science < 0 || tax > 10 || science > 10 || tax + science > 10)
      return ErrJson("illegal", "税率和科学率须在 0 到 10 之间且合计不超过 10");
    civ->tax_rate = static_cast<std::uint8_t>(tax);
    civ->science_rate = static_cast<std::uint8_t>(science);
    int nc = G()->total_cities;
    for (int i = 0; i < nc; ++i) {
      if (Cities()[i].id && Cities()[i].owner == Human()) {
        if (SehCalc(i)) return ErrJson("game_fault", "CalcCityGlobals 调用异常");
      }
    }
    RefreshShadow();
    RepaintPanels();
    return OkJson(std::string("\"tax_rate\":") + std::to_string(tax) +
                  ",\"science_rate\":" + std::to_string(science));
  }

  return ErrJson("illegal", "未知动作");
}

std::string GameBeginEndTurnJson() {
  std::string err;
  if (!RequireMatch(err)) return err;
  if (DialogPtr()) {
    return std::string("{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"已有对话框\",") +
           "\"dialog\":" + DialogJson() + "}";
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
  if (g_pending.kind == PendingKind::EndTurn && !DialogPtr()) {
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

void GameCancelPending() { g_pending = {}; }

void GameWakeUi(bool press_enter, bool press_escape) {
  static DWORD last_log = 0;
  DWORD now = GetTickCount();
  if (now - last_log > 2000) {
    last_log = now;
    GameLog("wake ui enter=%d escape=%d pending=%d turn=%d", press_enter ? 1 : 0,
            press_escape ? 1 : 0, static_cast<int>(g_pending.kind),
            MatchLive() ? G()->turn : -1);
  }
  LPARAM flags = 0;
  if (press_enter) flags |= kKickEnter;
  if (press_escape) flags |= kKickEscape;
  EnumWindows(KickWindow, flags);
}
