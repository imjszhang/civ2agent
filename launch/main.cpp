#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::wstring ExeDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir = path;
  auto slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return dir;
}

std::wstring Parent(const std::wstring& dir) {
  auto slash = dir.find_last_of(L"\\/");
  if (slash == std::wstring::npos || slash == 0) return dir;
  return dir.substr(0, slash);
}

bool Exists(const std::wstring& path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (n <= 0) return L"";
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}

bool ReadFileBytes(const std::wstring& path, std::string* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(file, nullptr);
  if (size == INVALID_FILE_SIZE || size > (1u << 20)) {
    CloseHandle(file);
    return false;
  }
  out->assign(size, '\0');
  DWORD got = 0;
  BOOL ok = size == 0 || ReadFile(file, out->data(), size, &got, nullptr);
  CloseHandle(file);
  if (!ok) return false;
  out->resize(got);
  if (out->size() >= 3 && static_cast<unsigned char>((*out)[0]) == 0xEF &&
      static_cast<unsigned char>((*out)[1]) == 0xBB && static_cast<unsigned char>((*out)[2]) == 0xBF)
    out->erase(0, 3);
  return true;
}

// The "game" string in game.json. Only that one key is read.
std::wstring GameFromJson(const std::string& text) {
  auto key = text.find("\"game\"");
  if (key == std::string::npos) return L"";
  auto colon = text.find(':', key + 6);
  if (colon == std::string::npos) return L"";
  auto quote = text.find('"', colon + 1);
  if (quote == std::string::npos) return L"";
  std::string value;
  for (size_t i = quote + 1; i < text.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"') return Utf8ToWide(value);
    if (c != '\\') {
      value.push_back(static_cast<char>(c));
      continue;
    }
    if (++i >= text.size()) return L"";
    char n = text[i];
    if (n == 'u') {
      if (i + 4 >= text.size()) return L"";
      int cp = 0;
      for (int k = 0; k < 4; ++k) {
        char h = text[++i];
        cp <<= 4;
        if (h >= '0' && h <= '9') cp += h - '0';
        else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
        else return L"";
      }
      if (cp < 0x80) value.push_back(static_cast<char>(cp));
      else if (cp < 0x800) {
        value.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        value.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else {
        value.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        value.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        value.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    } else if (n == '\\' || n == '/' || n == '"') {
      value.push_back(n);
    } else {
      return L"";
    }
  }
  return L"";
}

std::wstring FindFileUp(const std::wstring& start, const wchar_t* name, int limit) {
  std::wstring dir = start;
  for (int i = 0; i < limit; ++i) {
    std::wstring file = dir + L"\\" + name;
    if (Exists(file)) return file;
    std::wstring up = Parent(dir);
    if (up == dir) break;
    dir = up;
  }
  return L"";
}

// The directory named in game.json, or empty when no file is found.
// A file that cannot be read sets *broken and returns empty.
std::wstring ConfiguredGame(const std::wstring& start, bool* broken) {
  *broken = false;
  std::wstring file = FindFileUp(start, L"game.json", 6);
  if (file.empty()) return L"";
  std::string bytes;
  if (!ReadFileBytes(file, &bytes)) {
    wprintf(L"game.json 读不了：%ls\n", file.c_str());
    *broken = true;
    return L"";
  }
  std::wstring game = GameFromJson(bytes);
  if (game.empty()) {
    wprintf(L"game.json 须有 \"game\"：civ2.exe 所在目录\n");
    *broken = true;
    return L"";
  }
  if (game.find(L':') == std::wstring::npos && game.rfind(L"\\\\", 0) != 0) game = Parent(file) + L"\\" + game;
  return game;
}

std::wstring FindGameDir(const std::wstring& start) {
  std::wstring dir = start;
  for (int i = 0; i < 6; ++i) {
    if (Exists(dir + L"\\civ2.exe")) return dir;
    std::wstring up = Parent(dir);
    if (up == dir) break;
    dir = up;
  }
  wchar_t cwd[MAX_PATH] = {};
  GetCurrentDirectoryW(MAX_PATH, cwd);
  dir = cwd;
  for (int i = 0; i < 4; ++i) {
    if (Exists(dir + L"\\civ2.exe")) return dir;
    std::wstring up = Parent(dir);
    if (up == dir) break;
    dir = up;
  }
  return L"";
}

DWORD FindPid(const wchar_t* name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  DWORD pid = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, name) == 0) {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return pid;
}

bool Inject(DWORD pid, const std::wstring& dll) {
  HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                   PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
  if (!process) {
    wprintf(L"OpenProcess failed %lu\n", GetLastError());
    return false;
  }
  SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) {
    CloseHandle(process);
    return false;
  }
  if (!WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }
  auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                      "LoadLibraryW"));
  HANDLE thread = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
  if (!thread) {
    wprintf(L"CreateRemoteThread failed %lu\n", GetLastError());
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }
  WaitForSingleObject(thread, 20000);
  DWORD code = 0;
  GetExitCodeThread(thread, &code);
  CloseHandle(thread);
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  CloseHandle(process);
  if (!code) wprintf(L"LoadLibraryW returned NULL\n");
  return code != 0;
}

bool LaunchUia(const std::wstring& game) {
  std::wstring launcher = game + L"\\Civ2UIALauncher.exe";
  std::wstring exe = game + L"\\civ2.exe";
  std::wstring uia = game + L"\\Civ2UIA.dll";
  if (!Exists(launcher) || !Exists(uia)) {
    wprintf(L"缺少 Civ2UIALauncher.exe 或 Civ2UIA.dll\n");
    return false;
  }
  std::wstring cmd = L"\"" + launcher + L"\" -play -exe \"" + exe + L"\" -dll \"" + uia + L"\"";
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, game.c_str(), &si,
                      &pi)) {
    wprintf(L"启动 Civ2UIALauncher 失败 %lu\n", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring game;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--game") == 0 && i + 1 < argc) game = argv[++i];
  }
  if (game.empty()) {
    wchar_t env[32768] = {};
    DWORD n = GetEnvironmentVariableW(L"CIV2AGENT_GAME", env, 32768);
    if (n > 0 && n < 32768) game = env;
  }
  bool broken = false;
  if (game.empty()) game = ConfiguredGame(ExeDir(), &broken);
  if (game.empty() && !broken) {
    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwd)) game = ConfiguredGame(cwd, &broken);
  }
  if (broken) return 1;
  if (game.empty()) game = FindGameDir(ExeDir());
  if (game.empty() || !Exists(game + L"\\civ2.exe")) {
    wprintf(L"找不到 civ2.exe。用 --game、环境变量 CIV2AGENT_GAME 或 game.json 指定安装目录。\n");
    return 1;
  }
  if (WaitNamedPipeW(L"\\\\.\\pipe\\civ2agent", 300)) {
    wprintf(L"civ2agent 管道已在运行\n");
    return 0;
  }
  std::wstring dll = ExeDir() + L"\\civ2agent.dll";
  if (!Exists(dll)) {
    wprintf(L"找不到 %ls\n", dll.c_str());
    return 1;
  }
  DWORD pid = FindPid(L"civ2.exe");
  if (!pid) {
    wprintf(L"通过 Civ2UIA 启动 %ls\n", game.c_str());
    if (!LaunchUia(game)) return 1;
    for (int i = 0; i < 120 && !pid; ++i) {
      Sleep(500);
      pid = FindPid(L"civ2.exe");
    }
  } else {
    wprintf(L"civ2.exe 已在运行，直接注入\n");
  }
  if (!pid) {
    wprintf(L"没有等到 civ2.exe\n");
    return 1;
  }
  Sleep(800);
  if (!Inject(pid, dll)) {
    wprintf(L"注入失败\n");
    return 1;
  }
  wprintf(L"已注入 pid=%lu\n", pid);
  return 0;
}
