#include "game.h"
#include "jsonutil.h"

#include <windows.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\civ2agent";

using DispatchFn = LRESULT(WINAPI*)(const MSG*);
DispatchFn g_orig_dispatch = nullptr;
int g_depth = 0;
bool g_abort_on_dialog = false;

struct Job {
  int id = 0;
  std::string op;
  std::string name;
  std::string kind;
  std::string order;
  bool debug = false;
  int unit = -1;
  int city = -1;
  int x = 0;
  int y = 0;
  int tech = -1;
  int tax = -1;
  int science = -1;
  int button = -1;
  int timeout_ms = 8000;
  std::string response;
  bool done = false;
  bool abandoned = false;
  std::mutex mu;
  std::condition_variable cv;
};

std::mutex g_qmu;
std::deque<std::shared_ptr<Job>> g_queue;
std::shared_ptr<Job> g_running;

bool IsDone(const std::shared_ptr<Job>& job) {
  std::lock_guard<std::mutex> lock(job->mu);
  return job->done;
}

bool IsAbandoned(const std::shared_ptr<Job>& job) {
  std::lock_guard<std::mutex> lock(job->mu);
  return job->abandoned;
}

void FinishJob(const std::shared_ptr<Job>& job, const std::string& response) {
  std::lock_guard<std::mutex> lock(job->mu);
  if (job->done || job->abandoned) {
    job->done = true;
    return;
  }
  job->response = response;
  job->done = true;
  job->cv.notify_all();
}

std::string WithId(int id, const std::string& obj) {
  if (obj.empty() || obj[0] != '{') return obj;
  return std::string("{\"id\":") + std::to_string(id) + "," + obj.substr(1);
}

bool SafeOp(const Job& job) {
  if (job.op == "hello" || job.op == "snapshot" || job.op == "events" || job.op == "menu") return true;
  if (job.op == "act" && (job.name == "respond" || job.name == "menu")) return true;
  return false;
}

void ExecuteJob(const std::shared_ptr<Job>& job) {
  if (job->op == "hello") {
    FinishJob(job, WithId(job->id, GameHelloJson()));
    return;
  }
  if (job->op == "snapshot") {
    FinishJob(job, WithId(job->id, GameSnapshotJson(job->debug)));
    return;
  }
  if (job->op == "events") {
    FinishJob(job, WithId(job->id, GameEventsJson()));
    return;
  }
  if (job->op == "menu") {
    FinishJob(job, WithId(job->id, GameMenuJson()));
    return;
  }
  if (job->op == "act") {
    g_abort_on_dialog = job->name != "respond";
    std::string resp = GameActJson(job->name, job->unit, job->city, job->x, job->y, job->tech, job->tax,
                                   job->science, job->button, job->kind, job->order);
    g_abort_on_dialog = false;
    if (!IsDone(job)) FinishJob(job, WithId(job->id, resp));
    return;
  }
  if (job->op == "end_turn") {
    g_abort_on_dialog = true;
    std::string early = GameBeginEndTurnJson();
    if (!early.empty()) {
      g_abort_on_dialog = false;
      FinishJob(job, WithId(job->id, early));
      return;
    }
    if (IsDone(job)) {
      g_abort_on_dialog = false;
      return;
    }
    if (GameEndTurnDone()) {
      g_abort_on_dialog = false;
      FinishJob(job, WithId(job->id, GameFinishEndTurnJson()));
    }
    return;
  }
  FinishJob(job, WithId(job->id, ErrJson("illegal", "未知 op")));
}

void Pump() {
  GameObserveDialog();
  if (g_running && IsAbandoned(g_running)) {
    GameCancelEndTurn();
    g_abort_on_dialog = false;
    g_running.reset();
  }
  if (g_running && !IsDone(g_running) && g_abort_on_dialog && GameDialogOpen()) {
    std::string resp;
    if (GameTakeModalAbort(resp)) {
      GameCancelEndTurn();
      g_abort_on_dialog = false;
      FinishJob(g_running, WithId(g_running->id, resp));
    }
  }
  if (g_running && !IsDone(g_running) && GameEndTurnPending() && GameEndTurnDone()) {
    g_abort_on_dialog = false;
    FinishJob(g_running, WithId(g_running->id, GameFinishEndTurnJson()));
  }
  if (g_running && IsDone(g_running)) g_running.reset();
  if (g_running) return;

  std::shared_ptr<Job> job;
  {
    std::lock_guard<std::mutex> lock(g_qmu);
    if (g_queue.empty()) return;
    if (g_depth > 1 && !SafeOp(*g_queue.front())) return;
    job = g_queue.front();
    g_queue.pop_front();
  }
  g_running = job;
  ExecuteJob(job);
  if (g_running == job && IsDone(job)) g_running.reset();
}

LRESULT WINAPI DispatchHook(const MSG* msg) {
  ++g_depth;
  if (g_depth == 1 || g_depth > 1) Pump();
  LRESULT result = g_orig_dispatch(msg);
  --g_depth;
  return result;
}

void WakeGame() {
  EnumWindows(
      [](HWND hwnd, LPARAM) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) PostMessageA(hwnd, WM_NULL, 0, 0);
        return TRUE;
      },
      0);
}

bool HookDispatch() {
  auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
  for (; imp->Name; ++imp) {
    const char* dll = reinterpret_cast<const char*>(base + imp->Name);
    if (_stricmp(dll, "USER32.dll") != 0 && _stricmp(dll, "user32.dll") != 0) continue;
    auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (strcmp(name->Name, "DispatchMessageA") != 0) continue;
      DWORD old = 0;
      VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
      g_orig_dispatch = reinterpret_cast<DispatchFn>(thunk->u1.Function);
      thunk->u1.Function = reinterpret_cast<ULONG_PTR>(DispatchHook);
      VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
      GameLog("hooked DispatchMessageA");
      return true;
    }
  }
  GameLog("DispatchMessageA import not found");
  return false;
}

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  bool Parse(Job& job, std::string& err) {
    Skip();
    if (!Eat('{')) {
      err = "请求不是 JSON 对象";
      return false;
    }
    Skip();
    if (Peek() == '}') return true;
    while (i_ < s_.size()) {
      Skip();
      std::string key;
      if (!ReadString(key)) {
        err = "JSON 键无效";
        return false;
      }
      Skip();
      if (!Eat(':')) {
        err = "JSON 缺少冒号";
        return false;
      }
      Skip();
      if (!ReadValue(key, job, err)) return false;
      Skip();
      if (Peek() == ',') {
        ++i_;
        continue;
      }
      if (Peek() == '}') return true;
      err = "JSON 对象没有正确结束";
      return false;
    }
    err = "JSON 被截断";
    return false;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  void Skip() {
    while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
  }
  char Peek() const { return i_ < s_.size() ? s_[i_] : 0; }
  bool Eat(char c) {
    if (Peek() != c) return false;
    ++i_;
    return true;
  }
  bool ReadString(std::string& out) {
    if (!Eat('"')) return false;
    out.clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c == '\\' && i_ < s_.size()) {
        char e = s_[i_++];
        if (e == 'n') out.push_back('\n');
        else if (e == 'r') out.push_back('\r');
        else if (e == 't') out.push_back('\t');
        else out.push_back(e);
      } else {
        out.push_back(c);
      }
    }
    return false;
  }
  bool ReadValue(const std::string& key, Job& job, std::string& err) {
    if (Peek() == '"') {
      std::string v;
      if (!ReadString(v)) {
        err = "字符串值无效";
        return false;
      }
      if (key == "op") job.op = v;
      else if (key == "name") job.name = v;
      else if (key == "kind") job.kind = v;
      else if (key == "order" || key == "text") job.order = v;
      return true;
    }
    if (Peek() == 't' || Peek() == 'f') {
      bool val = Peek() == 't';
      const char* lit = val ? "true" : "false";
      for (int n = 0; lit[n]; ++n) {
        if (Peek() != lit[n]) {
          err = "布尔值无效";
          return false;
        }
        ++i_;
      }
      if (key == "debug") job.debug = val;
      return true;
    }
    if (Peek() == 'n') {
      for (char c : {'n', 'u', 'l', 'l'}) {
        if (!Eat(c)) {
          err = "null 无效";
          return false;
        }
      }
      return true;
    }
    char* end = nullptr;
    long v = strtol(s_.c_str() + i_, &end, 10);
    if (end == s_.c_str() + i_) {
      err = "值无效";
      return false;
    }
    i_ = static_cast<size_t>(end - s_.c_str());
    if (key == "id") job.id = static_cast<int>(v);
    else if (key == "unit") job.unit = static_cast<int>(v);
    else if (key == "city") job.city = static_cast<int>(v);
    else if (key == "x") job.x = static_cast<int>(v);
    else if (key == "y") job.y = static_cast<int>(v);
    else if (key == "tech" || key == "item") job.tech = static_cast<int>(v);
    else if (key == "tax") job.tax = static_cast<int>(v);
    else if (key == "science") job.science = static_cast<int>(v);
    else if (key == "button") job.button = static_cast<int>(v);
    else if (key == "timeout_ms") job.timeout_ms = static_cast<int>(v);
    return true;
  }
};

void ServeLoop() {
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1 << 20,
                                   1 << 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      GameLog("CreateNamedPipe failed %lu", GetLastError());
      Sleep(500);
      continue;
    }
    BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }
    GameLog("client connected");
    std::string acc;
    char buf[4096];
    for (;;) {
      DWORD got = 0;
      BOOL ok = ReadFile(pipe, buf, sizeof(buf), &got, nullptr);
      if (!ok || got == 0) break;
      acc.append(buf, buf + got);
      for (;;) {
        auto nl = acc.find('\n');
        if (nl == std::string::npos) break;
        std::string line = acc.substr(0, nl);
        acc.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto job = std::make_shared<Job>();
        Parser parser(line);
        std::string perr;
        if (!parser.Parse(*job, perr)) {
          std::string resp = WithId(0, ErrJson("illegal", perr)) + "\n";
          DWORD wrote = 0;
          WriteFile(pipe, resp.data(), static_cast<DWORD>(resp.size()), &wrote, nullptr);
          continue;
        }
        if (job->op == "end-turn") job->op = "end_turn";
        if (job->op == "end_turn" && job->timeout_ms == 8000) job->timeout_ms = 120000;
        {
          std::lock_guard<std::mutex> lock(g_qmu);
          g_queue.push_back(job);
        }
        WakeGame();
        std::string resp;
        {
          std::unique_lock<std::mutex> lock(job->mu);
          bool signaled = job->cv.wait_for(lock, std::chrono::milliseconds(job->timeout_ms),
                                           [&] { return job->done; });
          if (!signaled) {
            job->abandoned = true;
            job->done = true;
            resp = WithId(job->id, ErrJson("timeout", "游戏线程没有在时限内完成命令"));
          } else if (job->abandoned) {
            resp = WithId(job->id, ErrJson("timeout", "游戏线程没有在时限内完成命令"));
          } else {
            resp = job->response;
          }
        }
        resp.push_back('\n');
        DWORD wrote = 0;
        WriteFile(pipe, resp.data(), static_cast<DWORD>(resp.size()), &wrote, nullptr);
      }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    GameLog("client disconnected");
  }
}

}  // namespace

void StartServer() {
  if (!HookDispatch()) GameLog("continuing without dispatch hook");
  HANDLE thread = CreateThread(
      nullptr, 0, [](LPVOID) -> DWORD { ServeLoop(); return 0; }, nullptr, 0, nullptr);
  if (thread) CloseHandle(thread);
  else GameLog("pipe thread failed %lu", GetLastError());
}
