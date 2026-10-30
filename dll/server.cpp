#include "game.h"
#include "jsonutil.h"
#include "sysdlg.h"

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
  ActArgs act;
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
  if (job.op == "hello" || job.op == "snapshot" || job.op == "events" || job.op == "menu" ||
      job.op == "skip_intro")
    return true;
  if (job.op != "act") return false;
  if (job.act.name == "respond") return true;
  // A window the game holds open takes clicks on its own controls, as a
  // dialog takes answers: the city window, the tax rate window.
  if (GameDialogOpen()) return false;
  if (job.act.name == "produce" || job.act.name == "close_city") return GameCityWindowOpen();
  if (job.act.name == "rates") return GameTaxWindowOpen();
  return false;
}

// These acts expect dialogs: respond answers one, produce and rates go
// through the game's own windows and tell their dialogs from unexpected ones.
bool HandlesDialogs(const std::string& name) {
  return name == "respond" || name == "produce" || name == "rates";
}

void ExecuteJob(const std::shared_ptr<Job>& job) {
  if (job->op == "hello") {
    FinishJob(job, WithId(job->id, GameHelloJson()));
    return;
  }
  if (job->op == "snapshot") {
    FinishJob(job, WithId(job->id, GameSnapshotJson()));
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
  if (job->op == "skip_intro") {
    std::string resp = GameBeginSkipIntroJson();
    if (resp.empty() && GamePendingActive()) return;
    if (!IsDone(job)) FinishJob(job, WithId(job->id, resp));
    return;
  }
  if (job->op == "act" || job->op == "end_turn") {
    g_abort_on_dialog = job->op == "end_turn" || !HandlesDialogs(job->act.name);
    std::string resp = job->op == "end_turn" ? GameBeginEndTurnJson() : GameActJson(job->act);
    if (resp.empty() && GamePendingActive()) return;
    g_abort_on_dialog = false;
    if (!IsDone(job)) FinishJob(job, WithId(job->id, resp));
    return;
  }
  FinishJob(job, WithId(job->id, ErrJson("illegal", "未知 op")));
}

void Pump() {
  GameCacheGate();
  GameObserveDialog();
  if (g_running && IsAbandoned(g_running)) {
    GameCancelPending();
    g_abort_on_dialog = false;
    g_running.reset();
  }
  if (g_running && !IsDone(g_running) && g_abort_on_dialog &&
      (GameDialogOpen() || GameWindowModal() || GameModalScreenOpen() || GameMovieOpen() || GameUnregisteredLock())) {
    std::string resp;
    if (GameTakeModalAbort(resp)) {
      GameCancelPending();
      g_abort_on_dialog = false;
      FinishJob(g_running, WithId(g_running->id, resp));
    }
  }
  if (g_running && !IsDone(g_running) && GamePendingActive()) {
    std::string resp = GamePollPending(g_depth);
    // The poll can run a game loop that finishes the job (and resets
    // g_running) further down, before it returns here.
    if (!resp.empty() && g_running && !IsDone(g_running)) {
      g_abort_on_dialog = false;
      FinishJob(g_running, WithId(g_running->id, resp));
    }
  }
  if (g_running && IsDone(g_running)) g_running.reset();
  if (g_running) return;

  std::shared_ptr<Job> job;
  bool blocked_by_dialog = false;
  {
    std::lock_guard<std::mutex> lock(g_qmu);
    if (g_queue.empty()) return;
    if (g_depth > 1 && !SafeOp(*g_queue.front())) {
      // Inside a dialog's own loop a game command cannot run. Answer it with
      // the dialog now: left queued it would time out and run late, and
      // end_turn's wake-up Enter would confirm a dialog the agent never saw.
      if (!GameDialogOpen() && !GameWindowModal() && !GameModalScreenOpen() && !GameMovieOpen() &&
          !GameUnregisteredLock())
        return;
      blocked_by_dialog = true;
    }
    job = g_queue.front();
    g_queue.pop_front();
  }
  if (IsAbandoned(job)) return;  // the client already got a timeout
  if (blocked_by_dialog) {
    std::string resp;
    if (!GameTakeModalAbort(resp)) resp = ErrJson("modal_open", "出现对话框");
    FinishJob(job, WithId(job->id, resp));
    return;
  }
  g_running = job;
  ExecuteJob(job);
  if (g_running == job && IsDone(job)) g_running.reset();
}

// Takes a request back out of the queue before the game thread starts it.
bool Withdraw(const std::shared_ptr<Job>& job) {
  std::lock_guard<std::mutex> lock(g_qmu);
  for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
    if (*it == job) {
      g_queue.erase(it);
      return true;
    }
  }
  return false;
}

// The answer while a Windows dialog holds the game thread, from the pipe
// thread: status and dialog report it, respond answers it, the rest waits.
std::string SystemDialogReply(const Job& job, const std::string& dialog) {
  if (job.op == "hello") return GameHelloDuringSysDialog();
  std::string err;
  if (GameBlockedCached(err)) return err;
  if (job.op == "menu") return GameDialogDuringSysDialog(dialog);
  if (job.op == "act" && job.act.name == "respond") {
    if (!job.act.windows)
      return "{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"打开的是 Windows 对话框，不是刚才那个，先用 dialog "
             "看一下\",\"dialog\":" + dialog + "}";
    if (job.act.has_edit) return ErrJson("illegal", "Windows 对话框没有游戏的输入框，文件名用 file");
    return SysDialogRespond(job.act.option, job.act.text, job.act.has_file ? &job.act.file : nullptr);
  }
  return "{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"Windows 对话框（保存或打开文件）在等回答，先 respond\","
         "\"dialog\":" + dialog + "}";
}

// A wonder or throne still holds the game thread. Answer from this thread:
// a queued command would sit until the click, and the click has to be sent
// from here because the game thread is the one blocked in the wait.
std::string FilmReply(const Job& job) {
  if (job.op == "hello") return GameHelloJson();
  if (job.op == "menu") return GameMenuJson();
  if (job.op == "snapshot") return GameSnapshotJson();
  if (job.op == "events") return GameEventsJson();
  if (job.op == "act" && job.act.name == "respond") {
    if (job.act.has_edit) return ErrJson("illegal", "这个窗口没有输入框");
    bool next = job.act.text.empty() ? job.act.option == 0 : job.act.text == "继续";
    if (!next) return ErrJson("illegal", "这个窗口只有「继续」");
    return GameClickFilm();
  }
  std::string resp;
  if (!GameTakeModalAbort(resp)) resp = ErrJson("modal_open", "奇观动画在等鼠标，用 respond 继续");
  return resp;
}

LRESULT WINAPI DispatchHook(const MSG* msg) {
  ++g_depth;
  if (g_depth == 1 || g_depth > 1) Pump();
  LRESULT result = g_orig_dispatch(msg);
  GameReleaseMovieClick();
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

// Civ2UIA runs its own wait loop while the game sits on the end-of-turn
// prompt, and that loop dispatches through Civ2UIA's imports, not civ2.exe's.
bool HookModule(HMODULE module) {
  auto base = reinterpret_cast<std::uint8_t*>(module);
  if (!base) return false;
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
  bool hooked = false;
  for (; imp->Name; ++imp) {
    const char* dll = reinterpret_cast<const char*>(base + imp->Name);
    if (_stricmp(dll, "USER32.dll") != 0 && _stricmp(dll, "user32.dll") != 0) continue;
    // Matching the bound address works for Civ2UIA too, whose import table has
    // no name thunks.
    auto real = reinterpret_cast<ULONG_PTR>(GetProcAddress(GetModuleHandleA("user32.dll"), "DispatchMessageA"));
    auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; thunk->u1.Function; ++thunk) {
      if (thunk->u1.Function == reinterpret_cast<ULONG_PTR>(DispatchHook)) {
        hooked = true;
        continue;
      }
      if (thunk->u1.Function != real) continue;
      DWORD old = 0;
      VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
      if (!g_orig_dispatch) g_orig_dispatch = reinterpret_cast<DispatchFn>(thunk->u1.Function);
      thunk->u1.Function = reinterpret_cast<ULONG_PTR>(DispatchHook);
      VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
      hooked = true;
    }
  }
  return hooked;
}

bool HookDispatch() {
  bool game = HookModule(GetModuleHandleA(nullptr));
  bool uia = HookModule(GetModuleHandleA("Civ2UIA.dll"));
  GameLog("DispatchMessageA hooks: civ2.exe=%d Civ2UIA.dll=%d", game, uia);
  return game;
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
  bool ReadHex4(unsigned& v) {
    if (i_ + 4 > s_.size()) return false;
    v = 0;
    for (int n = 0; n < 4; ++n) {
      char h = s_[i_++];
      v <<= 4;
      if (h >= '0' && h <= '9') v |= h - '0';
      else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
      else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
      else return false;
    }
    return true;
  }
  // \uXXXX (with surrogate pairs) to UTF-8; json.dumps escapes non-ASCII by default.
  bool ReadUnicodeEscape(std::string& out) {
    unsigned cp = 0;
    if (!ReadHex4(cp)) return false;
    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 2 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
      i_ += 2;
      unsigned low = 0;
      if (!ReadHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
      cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return true;
  }
  bool Eat(char c) {
    if (Peek() != c) return false;
    ++i_;
    return true;
  }
  bool ReadStringArray(std::vector<std::string>& items, std::string& err) {
    Eat('[');
    Skip();
    if (Eat(']')) return true;
    for (;;) {
      Skip();
      std::string v;
      if (!ReadString(v)) {
        err = "数组里只能是字符串";
        return false;
      }
      items.push_back(v);
      Skip();
      if (Eat(',')) continue;
      if (Eat(']')) return true;
      err = "数组没有正确结束";
      return false;
    }
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
        else if (e == 'u') {
          if (!ReadUnicodeEscape(out)) return false;
        } else out.push_back(e);
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
      else if (key == "name") job.act.name = v;
      else if (key == "order") job.act.order = v;
      else if (key == "text") job.act.text = v;
      else if (key == "item") job.act.item = v;
      else if (key == "edit") {
        job.act.edits = {v};
        job.act.has_edit = true;
      } else if (key == "file") {
        job.act.file = v;
        job.act.has_file = true;
      }
      return true;
    }
    if (Peek() == '[') {
      std::vector<std::string> items;
      if (!ReadStringArray(items, err)) return false;
      if (key == "edits") {
        job.act.edits = items;
        job.act.has_edit = true;
      }
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
      if (key == "windows") job.act.windows = val;
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
    else if (key == "unit") job.act.unit = static_cast<int>(v);
    else if (key == "city") job.act.city = static_cast<int>(v);
    else if (key == "x") job.act.x = static_cast<int>(v);
    else if (key == "y") job.act.y = static_cast<int>(v);
    else if (key == "option") job.act.option = static_cast<int>(v);
    else if (key == "item_id") job.act.item_id = static_cast<int>(v);
    else if (key == "tax") job.act.tax = static_cast<int>(v);
    else if (key == "science") job.act.science = static_cast<int>(v);
    else if (key == "timeout_ms") job.timeout_ms = static_cast<int>(v);
    return true;
  }
};

void ServeLoop() {
  for (;;) {
    // Local clients only. There is no authentication: any local process can
    // connect and play.
    HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 1 << 20, 1 << 16, 0, nullptr);
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
        if (job->op == "skip-intro") job->op = "skip_intro";
        if (job->op == "end_turn" && job->timeout_ms == 8000) job->timeout_ms = 120000;
        if (job->op == "skip_intro" && job->timeout_ms == 8000) job->timeout_ms = 60000;
        std::string resp;
        // A Windows dialog holds the game thread: answer from here at once,
        // a queued request would only time out. A window that has no buttons
        // and no filename box yet is not a question; the caller reads again.
        std::string sys = SysDialogJson();
        std::string now_resp;
        if (SysDialogUnready())
          now_resp = "{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"Windows 对话框还没准备好，再读一次\",\"dialog\":null}";
        else if (!sys.empty()) now_resp = SystemDialogReply(*job, sys);
        else if (job->act.windows) now_resp = ErrJson("no_dialog", "Windows 对话框已经关了，先用 dialog 看一下");
        else if (GameMovieOpen()) now_resp = FilmReply(*job);
        if (!now_resp.empty()) {
          resp = WithId(job->id, now_resp) + "\n";
          DWORD wrote = 0;
          WriteFile(pipe, resp.data(), static_cast<DWORD>(resp.size()), &wrote, nullptr);
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(g_qmu);
          g_queue.push_back(job);
        }
        WakeGame();
        {
          std::unique_lock<std::mutex> lock(job->mu);
          auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(job->timeout_ms);
          bool signaled = false;
          bool withdrawn = false;
          bool unready_hold = false;
          bool film_hold = false;
          bool film_finished = false;
          while (!signaled) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            auto slice = deadline - now;
            if (slice > std::chrono::milliseconds(500)) slice = std::chrono::milliseconds(500);
            signaled = job->cv.wait_for(lock, slice, [&] { return job->done; });
            if (signaled) break;
            lock.unlock();
            bool unready = SysDialogUnready();
            sys = unready ? std::string() : SysDialogJson();
            // Not started yet, and a Windows dialog opened meanwhile: the game
            // thread cannot take it now.
            if (unready && Withdraw(job)) {
              withdrawn = true;
              unready_hold = true;
              lock.lock();
              break;
            }
            if (!sys.empty() && Withdraw(job)) {
              withdrawn = true;
              lock.lock();
              break;
            }
            // The turn command is already running and the game thread is
            // inside the still, so it will not reach Pump to abort itself.
            if (sys.empty() && GameMovieOpen()) {
              if (Withdraw(job)) {
                withdrawn = true;
                film_hold = true;
                lock.lock();
                break;
              }
              bool running = false;
              {
                std::lock_guard<std::mutex> qlock(g_qmu);
                running = g_running == job;
              }
              if (running) {
                std::string body;
                if (GameTakeModalAbort(body)) {
                  GameCancelPending();
                  g_abort_on_dialog = false;
                  FinishJob(job, WithId(job->id, body));
                  film_finished = true;
                }
              }
              if (film_finished) {
                lock.lock();
                break;
              }
              lock.lock();
              continue;
            }
            // Keys and clicks for the opening movie must not reach a file dialog.
            if (sys.empty()) GameWakeUi(job->op == "skip_intro");
            WakeGame();
            lock.lock();
          }
          if (withdrawn) {
            job->abandoned = true;
            job->done = true;
            resp = unready_hold ? WithId(job->id,
                                        "{\"ok\":false,\"error\":\"modal_open\",\"reason\":\"Windows 对话框还没准备好，再读一次\","
                                        "\"dialog\":null}")
                 : film_hold ? WithId(job->id, FilmReply(*job))
                                : WithId(job->id, SystemDialogReply(*job, sys));
          } else if (film_finished || signaled) {
            resp = job->abandoned ? WithId(job->id, ErrJson("timeout", "游戏线程没有在时限内完成命令"))
                                  : job->response;
          } else {
            job->abandoned = true;
            job->done = true;
            resp = WithId(job->id, ErrJson("timeout", "游戏线程没有在时限内完成命令"));
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
