#include "sysdlg.h"

#include "game.h"
#include "jsonutil.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace {

// Neither civ2.exe nor Civ2UIA.dll builds a dialog from a template (no
// CreateDialog or DialogBox imports): every #32770 window in the process is
// Windows' own, a file dialog or a message box.
constexpr wchar_t kDialogClass[] = L"#32770";
constexpr int kFileNameCombo = 0x47C;  // cmb13, the file name box of the Explorer dialog
constexpr int kFileNameEdit = 0x480;   // edt1, the file name box of the old dialog
constexpr int kGameNameBytes = 255;    // the game's buffer is 256 bytes, read through the ANSI API
constexpr UINT kSendTimeoutMs = 1000;

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

bool Wide(const std::string& utf8, std::wstring& out) {
  out.clear();
  if (utf8.empty()) return true;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return false;
  out.resize(n);
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return true;
}

// The game thread owns these windows; a hung owner must not hang the pipe.
std::wstring Text(HWND hwnd) {
  DWORD_PTR length = 0;
  if (!SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, kSendTimeoutMs, &length))
    return {};
  std::wstring buf(static_cast<size_t>(length) + 1, L'\0');
  DWORD_PTR got = 0;
  if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, buf.size(), reinterpret_cast<LPARAM>(buf.data()),
                           SMTO_ABORTIFHUNG | SMTO_BLOCK, kSendTimeoutMs, &got))
    return {};
  buf.resize(static_cast<size_t>(got));
  return buf;
}

std::wstring ClassOf(HWND hwnd) {
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  return cls;
}

// A caption as it reads on screen: "保存(&S)" shows as "保存(S)".
std::string Caption(const std::wstring& raw) {
  std::wstring out;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == L'&') {
      if (i + 1 < raw.size() && raw[i + 1] == L'&') out += L'&', ++i;
      continue;
    }
    out += raw[i];
  }
  return Utf8(out);
}

struct Button {
  HWND hwnd = nullptr;
  int id = 0;
  bool enabled = true;
  std::string text;
  int left = 0;
};

struct SysDialog {
  HWND hwnd = nullptr;
  bool enabled = true;
  bool file = false;  // a file dialog, with a name box
  HWND name_box = nullptr;
  std::string title;
  std::string message;
  std::vector<Button> buttons;
};

struct TopFind {
  HWND enabled = nullptr;
  HWND disabled = nullptr;
};

// A message box over the file dialog disables the file dialog; the enabled
// one is the one waiting for an answer.
BOOL CALLBACK FindTop(HWND hwnd, LPARAM lp) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
  if (ClassOf(hwnd) != kDialogClass) return TRUE;
  auto* find = reinterpret_cast<TopFind*>(lp);
  if (IsWindowEnabled(hwnd)) {
    if (!find->enabled) find->enabled = hwnd;
  } else if (!find->disabled) {
    find->disabled = hwnd;
  }
  return TRUE;
}

struct Controls {
  SysDialog* dialog;
  HWND first_edit = nullptr;
};

BOOL CALLBACK CollectControls(HWND hwnd, LPARAM lp) {
  auto* c = reinterpret_cast<Controls*>(lp);
  if (!IsWindowVisible(hwnd)) return TRUE;
  std::wstring cls = ClassOf(hwnd);
  int id = GetDlgCtrlID(hwnd);
  if (cls == L"Edit") {
    if (!c->first_edit) c->first_edit = hwnd;
    // The Explorer dialog nests the box: Edit in ComboBox in ComboBoxEx32 (cmb13).
    HWND up = GetParent(hwnd);
    HWND up2 = up ? GetParent(up) : nullptr;
    bool named = id == kFileNameEdit || (up && GetDlgCtrlID(up) == kFileNameCombo) ||
                 (up2 && GetDlgCtrlID(up2) == kFileNameCombo);
    if (named && !c->dialog->name_box) c->dialog->name_box = hwnd;
    return TRUE;
  }
  if (cls == L"Button") {
    LONG style = GetWindowLongW(hwnd, GWL_STYLE) & 0x0F;
    // Push buttons and split buttons (the Open button can be one).
    bool push = style == BS_PUSHBUTTON || style == BS_DEFPUSHBUTTON || style == 0x0C || style == 0x0D;
    if (!push) return TRUE;
    std::string text = Caption(Text(hwnd));
    if (text.empty()) return TRUE;
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    c->dialog->buttons.push_back({hwnd, id, IsWindowEnabled(hwnd) != FALSE, text, rect.left});
    return TRUE;
  }
  // The message of a message box (0xFFFF); 0x14 is its icon.
  if (cls == L"Static" && id == 0xFFFF && c->dialog->message.empty()) {
    c->dialog->message = Utf8(Text(hwnd));
  }
  return TRUE;
}

bool ReadSysDialog(SysDialog& out) {
  TopFind find;
  EnumWindows(FindTop, reinterpret_cast<LPARAM>(&find));
  HWND hwnd = find.enabled ? find.enabled : find.disabled;
  if (!hwnd) return false;
  out = {};
  out.hwnd = hwnd;
  out.enabled = IsWindowEnabled(hwnd) != FALSE;
  out.title = Utf8(Text(hwnd));
  Controls c{&out};
  EnumChildWindows(hwnd, CollectControls, reinterpret_cast<LPARAM>(&c));
  if (!out.name_box) out.name_box = c.first_edit;
  out.file = out.name_box != nullptr;
  std::stable_sort(out.buttons.begin(), out.buttons.end(),
                   [](const Button& a, const Button& b) { return a.left < b.left; });
  static HWND logged = nullptr;
  if (hwnd != logged) {
    logged = hwnd;
    GameLog("windows dialog %p file=%d name_box=%p id=%d buttons=%d", hwnd, out.file ? 1 : 0, out.name_box,
            out.name_box ? GetDlgCtrlID(out.name_box) : 0, static_cast<int>(out.buttons.size()));
  }
  return true;
}

}  // namespace

bool DialogUnready(const SysDialog& d) { return !d.file && d.buttons.empty(); }

bool SysDialogUnready() {
  SysDialog d;
  return ReadSysDialog(d) && DialogUnready(d);
}

std::string SysDialogJson() {
  SysDialog d;
  if (!ReadSysDialog(d) || DialogUnready(d)) return {};
  std::string out = "{\"title\":" + JsonString(d.title) + ",\"text\":" + JsonString(d.message) +
                    ",\"kind\":" + JsonString(d.file ? "file" : "message") + ",\"via\":\"windows\"";
  if (d.file) out += ",\"file\":" + JsonString(Utf8(Text(d.name_box)));
  out += ",\"options\":[";
  for (size_t i = 0; i < d.buttons.size(); ++i) {
    const Button& b = d.buttons[i];
    if (i) out += ',';
    out += "{\"index\":" + std::to_string(i) + ",\"text\":" + JsonString(b.text) + ",\"button\":" + std::to_string(b.id);
    if (!b.enabled) out += ",\"disabled\":true";
    out += "}";
  }
  return out + "]}";
}

std::string SysDialogRespond(int option, const std::string& text, const std::string* file) {
  SysDialog d;
  if (!ReadSysDialog(d)) return ErrJson("no_dialog", "没有打开的 Windows 对话框");
  if (!d.enabled) return ErrJson("no_dialog", "这个对话框上面还有一个对话框，先回答上面那个");
  if (file && !d.file) return ErrJson("illegal", "这个对话框没有文件名框");
  int index = option;
  if (!text.empty()) {
    index = -1;
    for (size_t i = 0; i < d.buttons.size(); ++i) {
      if (d.buttons[i].text == text) index = static_cast<int>(i);
    }
    if (index < 0) return ErrJson("illegal", "对话框上没有这个按钮");
  } else if (index < 0 && file) {
    for (size_t i = 0; i < d.buttons.size(); ++i) {
      if (d.buttons[i].id == IDOK) index = static_cast<int>(i);
    }
    if (index < 0) return ErrJson("no_dialog", "找不到保存或打开按钮");
  }
  if (index < 0 || index >= static_cast<int>(d.buttons.size())) return ErrJson("illegal", "选项下标超出范围");
  const Button& button = d.buttons[index];
  if (!button.enabled) return ErrJson("illegal", "这个按钮是灰的");
  if (file) {
    std::wstring name;
    if (!Wide(*file, name) || name.empty()) return ErrJson("illegal", "文件名不是有效的 UTF-8 文字");
    for (wchar_t ch : name) {
      if (ch < 0x20) return ErrJson("illegal", "文件名不能含控制字符");
    }
    BOOL lossy = FALSE;
    int bytes = WideCharToMultiByte(CP_ACP, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, &lossy);
    if (bytes <= 0 || lossy) return ErrJson("illegal", "文件名里有游戏不支持的字符");
    if (bytes > kGameNameBytes) return ErrJson("illegal", "文件名太长：游戏最多收 255 字节（一个汉字 2 字节）");
    DWORD_PTR done = 0;
    SendMessageTimeoutW(d.name_box, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(name.c_str()), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                        kSendTimeoutMs, &done);
    if (Text(d.name_box) != name) return ErrJson("illegal", "文件名框没有收下这段文字");
  }
  // Posted, as a click is: a question the button raises (replace the file?)
  // must find this thread free to answer it.
  PostMessageW(GetParent(button.hwnd), WM_COMMAND, MAKEWPARAM(button.id, BN_CLICKED),
               reinterpret_cast<LPARAM>(button.hwnd));
  return OkJson("\"index\":" + std::to_string(index) + ",\"text\":" + JsonString(button.text) + ",\"via\":\"windows\"");
}
