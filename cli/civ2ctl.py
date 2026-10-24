#!/usr/bin/env python3
"""Command line interface that agents use to observe and operate Civilization II.

Every call prints one JSON object on one line of stdout, always with "ok".
Failures add "error" (a fixed code) and "reason". Exit codes: 0 ok, 1 the game
refused or failed, 2 bad arguments, 3 the game is not reachable.

The commands, their help, `tools` (JSON Schema tool definitions) and `call`
(run a command with a JSON object of arguments) are all generated from
COMMANDS, so they cannot disagree. Each action is one thing a player can do in
the game's own interface; the DLL refuses what the interface would not allow.
save-game, load-game and quit-game are such actions too, made of the clicks a
player makes in the game's menu, Windows' file dialog and the game's replies.

While a game session is active (civ2session.py), every call is also appended
to that session's log.jsonl.
"""

import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")

PIPE = r"\\.\pipe\civ2agent"
PROTOCOL = 3
AGENT_ROOT = Path(__file__).resolve().parents[1]
GAME_ROOT = AGENT_ROOT.parent

EXIT_OK = 0
EXIT_REFUSED = 1
EXIT_USAGE = 2
EXIT_UNREACHABLE = 3

try:
    ctypes.windll.user32.SetProcessDPIAware()
except Exception:
    pass


class UsageError(Exception):
    pass


def fail(error, reason, **extra):
    response = {"ok": False, "error": error, "reason": reason}
    response.update(extra)
    return response


def exit_code(response):
    if response.get("ok"):
        return EXIT_OK
    if response.get("error") == "usage":
        return EXIT_USAGE
    if response.get("error") in ("not_connected", "timeout"):
        return EXIT_UNREACHABLE
    return EXIT_REFUSED


def games_dir():
    """Where agent-owned games live (civ2session.py); CIV2AGENT_GAMES_DIR overrides it."""
    return Path(os.environ.get("CIV2AGENT_GAMES_DIR") or AGENT_ROOT / "games")


def active_session():
    """The active session's lock record ({"id", "dir", ...}), or None."""
    try:
        record = json.loads((games_dir() / ".active").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return record if isinstance(record, dict) and record.get("dir") else None


def summarize(name, response):
    """What log.jsonl keeps of a response: state only in outline."""
    if name == "state" and response.get("ok") and response.get("in_game"):
        civ = response.get("civ") or {}
        return {"ok": True, "turn": response.get("turn"), "year": response.get("year"), "gold": civ.get("gold"),
                "units": len(response.get("units") or []), "cities": len(response.get("cities") or []),
                "dialog": (response.get("dialog") or {}).get("title")}
    return response


def log_session(name, req, response):
    session = active_session()
    if not session:
        return
    entry = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "command": name, "request": req,
             "response": summarize(name, response)}
    try:
        with open(Path(session["dir"]) / "log.jsonl", "a", encoding="utf-8") as handle:
            handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        pass  # the log must not change the command's outcome


def transact(payload, connect_s=10.0):
    """Send one request and return the parsed response. Only opening the pipe
    is retried; once a request is written it is never sent again, because the
    game may already be acting on it. The DLL bounds how long it takes."""
    data = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
    deadline = time.time() + connect_s
    while True:
        try:
            pipe = open(PIPE, "r+b", buffering=0)
            break
        except OSError as exc:
            if time.time() >= deadline:
                return fail("not_connected", f"连不上游戏：civ2.exe 没有运行，或 civ2agent 没有注入（{type(exc).__name__}）")
            time.sleep(0.25)
    buf = b""
    try:
        with pipe:
            pipe.write(data)
            while b"\n" not in buf:
                chunk = pipe.read(65536)
                if not chunk:
                    break
                buf += chunk
    except OSError as exc:
        return fail("not_connected", f"与游戏的连接中断：{exc}")
    line = buf.split(b"\n", 1)[0].decode("utf-8", errors="replace")
    if not line:
        return fail("not_connected", "游戏没有响应就断开了连接")
    try:
        response = json.loads(line)
    except ValueError:
        return fail("cli_fault", "游戏返回的不是 JSON", raw=line[:200])
    response.pop("id", None)
    return response


def root_window_at(x, y):
    user32 = ctypes.windll.user32
    user32.WindowFromPoint.argtypes = [wintypes.POINT]
    user32.WindowFromPoint.restype = wintypes.HWND
    user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
    user32.GetAncestor.restype = wintypes.HWND
    hit = user32.WindowFromPoint(wintypes.POINT(int(x), int(y)))
    return (user32.GetAncestor(hit, 2) or 0) if hit else 0  # GA_ROOT


def click_screen(x, y, hwnd, repeats=2):
    """Click with the real cursor, only while the point is on the dialog.
    Returns False without clicking when another window covers it."""
    user32 = ctypes.windll.user32
    if hwnd:
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.15)
    clicked = False
    for _ in range(max(1, repeats)):
        # A row can close the dialog on the first click; the second must not
        # land on whatever is underneath. Only the low 32 bits of a handle count.
        if not hwnd or (root_window_at(x, y) & 0xFFFFFFFF) != (hwnd & 0xFFFFFFFF):
            break
        user32.SetCursorPos(int(x), int(y))
        time.sleep(0.05)
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.05)
        user32.mouse_event(0x0004, 0, 0, 0, 0)
        clicked = True
        time.sleep(0.3)
    return clicked


def current_dialog():
    """The dialog on screen, or None. A failed query reads as no dialog."""
    response = transact({"op": "menu"}, connect_s=5)
    return response.get("dialog") if response.get("ok") else None


def dialog_key(dialog):
    if not dialog:
        return None
    return (dialog.get("title"), dialog.get("text"), tuple(o.get("text") for o in dialog.get("options") or []))


def choose(request, connect_s=10.0, settle_s=2.0):
    """Answer the dialog on screen and report what replaced it. The DLL answers
    most dialogs through the game's own handlers; for the rest it returns
    screen coordinates, because those controls ignore posted mouse input.
    Windows' own dialogs (file name, replace?) are answered with window
    messages; the request says which kind the caller saw, so an answer meant
    for one never presses a button in the other."""
    shown = current_dialog()
    before = dialog_key(shown)
    if shown and shown.get("via") == "windows":
        request = dict(request, windows=True)
    response = transact(request, connect_s=connect_s)
    click = response.pop("click", None)
    if not response.get("ok"):
        return response
    if click:
        if not click_screen(click["x"], click["y"], click.get("hwnd"), repeats=click.get("repeats", 1)):
            response.update(ok=False, error="window_covered", reason="游戏对话框被其他窗口挡住，没有点击")
            return response
        response["via"] = "mouse"
    deadline = time.time() + settle_s
    after = current_dialog()
    while dialog_key(after) == before and time.time() < deadline:
        time.sleep(0.2)
        after = current_dialog()
    if after is None:
        # The next dialog in a chain can open a moment after this one closes.
        time.sleep(0.4)
        after = current_dialog()
    response["changed"] = dialog_key(after) != before
    response["dialog"] = after
    return response


@dataclass
class Param:
    name: str
    kind: str  # "integer", "string", "boolean" or "strings" (a repeatable flag)
    help: str
    required: bool = False
    choices: tuple = ()
    default: object = None
    tool: bool = True  # False: a command line convenience, left out of the tool schema
    wire: str = None  # the pipe field when it differs from name; "" keeps it out of the request

    @property
    def flag(self):
        return "--" + self.name.replace("_", "-")


@dataclass
class Command:
    name: str
    kind: str  # "observe", "act" or "session"
    help: str
    params: tuple = ()
    build: object = None  # (command, args) -> pipe request
    answers_dialog: bool = False
    connect_s: float = 10.0
    local: object = None  # (args, connect_s) -> (request, response), for commands made of several requests
    after: object = None  # (args, response) -> response shown to the caller


def request(op, name=None, **fixed):
    """A builder that copies the command's tool parameters into a pipe request."""

    def build(command, args):
        req = {"op": op}
        if name:
            req["name"] = name
        req.update(fixed)
        for param in command.params:
            value = getattr(args, param.name, None)
            field = param.name if param.wire is None else param.wire
            if param.tool and field and value is not None and value is not False:
                req[field] = value
        return req

    return build


def build_respond(command, args):
    if args.option is not None and args.text is not None:
        raise UsageError("respond 的 --option 和 --text 只能给一个")
    if args.option is None and args.text is None and args.file is None:
        raise UsageError("respond 需要 --option 或 --text（Windows 文件对话框也可以只给 --file）")
    return request("act", "respond")(command, args)


STATE_FIELDS = ("civ", "units", "foreign_units", "cities", "foreign_cities", "tiles", "diplomacy",
                "city_window", "tax_window", "notice", "dialog")
STATE_ALWAYS = ("ok", "error", "reason", "in_game", "turn", "year", "human", "difficulty", "active_unit",
                "move_unit")


def build_state(command, args):
    for field in fields_of(args.only):
        if field not in STATE_FIELDS:
            raise UsageError(f"state --only 没有 {field}，可选：{','.join(STATE_FIELDS)}")
    return request("snapshot")(command, args)


def fields_of(text):
    return [f.strip() for f in (text or "").split(",") if f.strip()]


def keep_fields(args, response):
    """state --only: the named parts of the picture, plus turn and year."""
    wanted = fields_of(getattr(args, "only", None))
    if not wanted or not response.get("in_game"):
        return response
    return {k: v for k, v in response.items() if k in STATE_ALWAYS or k in wanted}


def build_produce(command, args):
    if args.item is not None and args.item_id is not None:
        raise UsageError("produce 的 --item 和 --item-id 只能给一个")
    return request("act", "produce", timeout_ms=20000)(command, args)


# Saving, loading and quitting, step by step as a player does them: a click
# in the game's menu, then Windows' file dialog or the game's confirmation,
# then the game's message. Each step goes through respond, so the screen
# shows every one of them.

def wait_dialog(test, timeout_s):
    """The first dialog on screen that test accepts, or None at the deadline."""
    deadline = time.time() + timeout_s
    while True:
        dialog = current_dialog()
        if dialog and test(dialog):
            return dialog
        if time.time() >= deadline:
            return None
        time.sleep(0.25)


def is_file_dialog(dialog):
    return dialog.get("via") == "windows" and dialog.get("kind") == "file"


def option_with(dialog, *texts):
    return next((o for o in dialog.get("options") or [] if o.get("text") in texts and not o.get("disabled")), None)


def press_ok(dialog):
    """Close a message that has only OK to press."""
    choice = option_with(dialog, "确定") or next(iter(dialog.get("options") or []), None)
    if not choice:
        return fail("no_dialog", "消息框上没有按钮")
    return choose({"op": "act", "name": "respond", "option": choice["index"]})


def answer_file_dialog(path, overwrite):
    """Type the name into Windows' file dialog and press its button; answer
    the replace question it can ask. Returns None when the dialog is gone."""
    response = choose({"op": "act", "name": "respond", "file": str(path)})
    if not response.get("ok"):
        return response
    after = response.get("dialog")
    if after and after.get("via") == "windows" and after.get("kind") == "message":
        # Windows asks before replacing a file; yes is button 6 in every language.
        yes = next((o for o in after.get("options") or [] if o.get("button") == 6), None)
        no = next((o for o in after.get("options") or [] if o.get("button") in (7, 2)), None)
        pick = yes if overwrite else no
        if not pick:
            return fail("illegal", "Windows 对话框问了一个不认识的问题", dialog=after)
        choose({"op": "act", "name": "respond", "option": pick["index"]})
        if not overwrite:
            return fail("illegal", "文件已经存在；要覆盖就加 --overwrite", dialog=current_dialog())
    return None


def save_game(path, overwrite=False, connect_s=10.0):
    """Game > 储存游戏, the file name, Save, then OK on the game's message."""
    path = Path(path)
    if not path.is_absolute() or path.suffix.upper() != ".SAV":
        return fail("usage", "存档文件名要用绝对路径，扩展名 .SAV")
    if not path.parent.is_dir():
        return fail("usage", f"目录不存在：{path.parent}")
    if path.exists() and not overwrite:
        return fail("illegal", "文件已经存在；要覆盖就加 --overwrite")
    started = time.time()
    posted = transact({"op": "act", "name": "save_game"}, connect_s=connect_s)
    if not posted.get("ok"):
        return posted
    dialog = wait_dialog(lambda d: is_file_dialog(d) or d.get("via") != "windows", 15)
    if not dialog or not is_file_dialog(dialog):
        return fail("game_fault", "游戏没有打开保存文件的对话框", dialog=dialog)
    refused = answer_file_dialog(path, overwrite)
    if refused:
        return refused
    message = wait_dialog(lambda d: d.get("via") != "windows", 30)
    if not message:
        return fail("timeout", "保存后游戏没有显示消息")
    title = message.get("title") or ""
    press_ok(message)
    if "存档完毕" not in title:
        return fail("game_fault", "游戏没有存成：" + (message.get("text") or title).strip(), dialog=message)
    if not path.exists() or path.stat().st_mtime < started - 2:
        return fail("game_fault", "游戏说存好了，但文件没有写出来")
    return {"ok": True, "file": str(path), "bytes": path.stat().st_size, "message": (message.get("text") or "").strip()}


def load_game(path, connect_s=10.0):
    """Game > 载入游戏 in a match, or 读取进度 on the main menu; the file name,
    Open, then OK on the game's message (困难程度: ...)."""
    path = Path(path)
    if not path.is_absolute() or not path.is_file():
        return fail("usage", "要读取的存档要用绝对路径，并且文件必须存在")
    status = transact({"op": "hello"}, connect_s=connect_s)
    if not status.get("ok"):
        return status
    if status.get("in_game"):
        posted = transact({"op": "act", "name": "load_game"}, connect_s=connect_s)
        if not posted.get("ok"):
            return posted
    else:
        menu = current_dialog() or {}
        row = next((o for o in menu.get("options") or [] if (o.get("text") or "").strip() == "读取进度"), None)
        if not row or not option_with(menu, "开始新游戏"):
            return fail("illegal", "不在主菜单，也不在对局中", dialog=menu or None)
        picked = choose({"op": "act", "name": "respond", "option": row["index"]})
        if not picked.get("ok"):
            return picked
    dialog = wait_dialog(lambda d: is_file_dialog(d) or d.get("via") != "windows", 15)
    if not dialog or not is_file_dialog(dialog):
        return fail("game_fault", "游戏没有打开读取文件的对话框", dialog=dialog)
    refused = answer_file_dialog(path, False)
    if refused:
        return refused
    message = wait_dialog(lambda d: d.get("via") != "windows", 60)
    if not message:
        return fail("timeout", "读取后游戏没有显示消息")
    title = message.get("title") or ""
    press_ok(message)
    if not title.startswith("困难程度"):
        return fail("game_fault", "游戏没有读进来：" + (message.get("text") or title).strip(), dialog=message)
    state = transact({"op": "snapshot"}, connect_s=connect_s)
    if not state.get("in_game"):
        return fail("game_fault", "读档后不在对局中", state=state)
    civ = state.get("civ") or {}
    return {"ok": True, "file": str(path), "turn": state.get("turn"), "year": state.get("year"),
            "civ": civ.get("name"), "leader": civ.get("leader"), "dialog": current_dialog()}


def to_main_menu(timeout_s=60):
    """Answer what comes between a match and the main menu the plain way."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        dialog = current_dialog()
        if dialog and option_with(dialog, "开始新游戏"):
            return dialog
        if dialog:
            choice = option_with(dialog, "取消", "确定") or next(iter(dialog.get("options") or []), None)
            if choice:
                choose({"op": "act", "name": "respond", "option": choice["index"]})
                continue
        time.sleep(0.4)
    return None


def game_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq civ2.exe", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, errors="replace", timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return "civ2.exe" in out.lower()


def quit_game(exit_program=False, connect_s=10.0):
    """Game > 离开游戏 and 是 at 确认; with exit_program, then 取消 on the main
    menu, which closes the game."""
    status = transact({"op": "hello"}, connect_s=connect_s)
    if not status.get("ok"):
        return status
    if status.get("in_game"):
        posted = transact({"op": "act", "name": "quit_game"}, connect_s=connect_s)
        if not posted.get("ok"):
            return posted
        confirm = wait_dialog(lambda d: option_with(d, "是") is not None, 15)
        if not confirm:
            return fail("game_fault", "游戏没有问是否离开")
        answered = choose({"op": "act", "name": "respond", "text": "是"})
        if not answered.get("ok"):
            return answered
    menu = to_main_menu()
    if not menu:
        return fail("timeout", "没有回到主菜单", dialog=current_dialog())
    if not exit_program:
        return {"ok": True, "at_menu": True, "exited": False}
    transact({"op": "act", "name": "respond", "text": "取消"}, connect_s=connect_s)
    deadline = time.time() + 30
    while game_running() and time.time() < deadline:
        time.sleep(0.5)
    if game_running():
        return fail("timeout", "按了取消，游戏还没有退出")
    return {"ok": True, "at_menu": False, "exited": True}


def session_guard(action):
    session = active_session()
    if not session:
        return None
    return fail("session_policy",
                f"游戏局 {session.get('id')} 正在进行，{action} 由 civ2session.py 管理，agent 不能自己做")


def local_save(args, connect_s):
    path = Path(args.file)
    session = active_session()
    if session:
        saves = (Path(session["dir"]) / "saves").resolve()
        path = path if path.is_absolute() else saves / path
        if path.resolve().parent != saves:
            return {"op": "save_game"}, fail("session_policy", f"游戏局进行中，只能存到 {saves}")
    return {"op": "save_game", "file": str(path)}, save_game(path, args.overwrite, connect_s)


def local_load(args, connect_s):
    refused = session_guard("读档")
    return {"op": "load_game", "file": args.file}, refused or load_game(args.file, connect_s)


def local_quit(args, connect_s):
    refused = session_guard("离开游戏")
    return {"op": "quit_game", "exit": args.exit}, refused or quit_game(args.exit, connect_s)


def local_wait_ready(args, connect_s):
    return {"op": "hello"}, wait_ready(args.in_game, args.wait_s)


UNIT = Param("unit", "integer", "己方单位的 index，取自 state 的 units", required=True)
TARGET_X = Param("x", "integer", "目标格的 x", required=True)
TARGET_Y = Param("y", "integer", "目标格的 y", required=True)
ORDERS = ("fortify", "sleep", "wait", "skip", "irrigate", "road", "mine", "clean")

COMMANDS = (
    Command("status", "observe", "连接状态：协议版本、civ2.exe 哈希、是否在对局中、是否有对话框。",
            build=request("hello"), connect_s=2.0),
    Command("state", "observe",
            "当前局面，只含玩家在画面上能看到的：己方文明、单位和城市，看得见的外国单位和城市，探索过的地形，外交状态，当前对话框。"
            "tiles 很大，用 --only 只取需要的部分。",
            params=(Param("only", "string", "只返回这些部分，逗号分隔：" + ",".join(STATE_FIELDS) +
                          "（turn、year 等总是返回）", wire=""),
                    Param("out", "string", "另存一份到这个 JSON 文件", tool=False)),
            build=build_state, after=keep_fields),
    Command("dialog", "observe", "当前对话框：标题、正文、可选项、输入框。没有对话框时 dialog 为 null。",
            build=request("menu")),
    Command("events", "observe", "取走上次以来的事件：对话框出现和关闭、建议框、己方单位损失或受伤、己方相关的城市易手、研究项变化。",
            build=request("events")),
    Command("respond", "act",
            "回答当前对话框，相当于玩家点选一项；先用 dialog 看选项。via 为 mouse 的对话框要用真实鼠标点，期间不要动鼠标。",
            params=(Param("option", "integer", "dialog.options 里的 index"),
                    Param("text", "string", "选项的文字，与 dialog.options 里的 text 完全一致"),
                    Param("edit", "strings", "先在输入框里填字再选，按输入框的顺序，可给多次（自定地图尺寸有横、纵两个）；"
                          "城市名最多 15 字节、领袖名 23 字节，汉字算 2 字节", wire="edits"),
                    Param("file", "string", "Windows 文件对话框（kind 为 file）：先在文件名框填这个路径；只给它时按保存或打开")),
            build=build_respond, answers_dialog=True),
    Command("activate", "act",
            "选中一个己方单位，相当于在地图上点它（会叫醒睡眠或设防的单位）。同一格有几个单位时游戏弹出单位列表，"
            "返回 modal_open，用 respond 选。",
            params=(UNIT,), build=request("act", "activate")),
    Command("move", "act", "让单位朝相邻一格走一步，相当于按一次方向键；可能触发战斗或对话框。",
            params=(UNIT, TARGET_X, TARGET_Y), build=request("act", "move")),
    Command("goto", "act", "让单位这一回合朝目标一步步走，每一步都是一次方向键，走不动就停下。",
            params=(UNIT, TARGET_X, TARGET_Y), build=request("act", "goto")),
    Command("order", "act", "给单位下命令，相当于点单位菜单里的一项；菜单里是灰的就拒绝。",
            params=(UNIT, Param("order", "string", "命令", required=True, choices=ORDERS)),
            build=request("act", "order")),
    Command("found-city", "act", "用拓荒者或工程师建城，相当于点「建立新城市」；接着弹出的命名框用 respond 回答。",
            params=(UNIT,), build=request("act", "found_city")),
    Command("produce", "act",
            "改城市的生产，和玩家一样：打开城市窗口，按「更改」，从游戏给的列表里选，再关上窗口。"
            "不给 --item 和 --item-id 时只返回可选项（options），不改生产。游戏弹出确认框时返回 modal_open，用 respond 回答。",
            params=(Param("city", "integer", "己方城市的 index，取自 state 的 cities", required=True),
                    Param("item", "string", "要造的项目，可选项里的 name"),
                    Param("item_id", "integer", "要造的项目，可选项里的 id")),
            build=build_produce),
    Command("close-city", "act",
            "关上城市窗口，相当于按窗口里的「退出」。游戏等城市窗口关上（city_window.modal 为 true）时用它继续。",
            build=request("act", "close_city")),
    Command("rates", "act",
            "设税收和科学比例（各 0 到 10，即 0% 到 100%，奢侈是剩下的），和玩家一样在税率窗口里拖滑块再确定；"
            "超过政体上限（max_rate）就拒绝，比例不变。游戏自己打开税率窗口时（tax_window 不为 null）也用它。",
            params=(Param("tax", "integer", "税收比例 0-10", required=True),
                    Param("science", "integer", "科学比例 0-10", required=True)),
            build=request("act", "rates", timeout_ms=20000)),
    Command("end-turn", "act", "结束回合，等到再次轮到玩家。中途有对话框时返回 modal_open，先 respond，再看 turn_ended。",
            params=(Param("timeout_ms", "integer", "最多等多少毫秒", default=120000),),
            build=request("end_turn")),
    Command("save-game", "act",
            "存档，和玩家一样：点「游戏」菜单的「储存游戏」，在 Windows 文件对话框里填文件名，按保存，再关掉「存档完毕!」。"
            "游戏局进行中只能存到这局的 saves 目录（可只给文件名）。",
            params=(Param("file", "string", "存档文件，绝对路径，扩展名 .SAV", required=True),
                    Param("overwrite", "boolean", "文件已存在时覆盖（Windows 会问一次，回答「是」）")),
            local=local_save),
    Command("load-game", "act",
            "读档：对局中点「游戏」菜单的「载入游戏」，在主菜单选「读取进度」；填文件名，按打开，再关掉读档的消息。游戏局进行中不能用。",
            params=(Param("file", "string", "存档文件，绝对路径", required=True),),
            local=local_load),
    Command("quit-game", "act",
            "离开游戏：点「游戏」菜单的「离开游戏」，在「确认」里选「是」，回到主菜单。游戏局进行中不能用。",
            params=(Param("exit", "boolean", "回到主菜单后再按取消，退出游戏程序"),),
            local=local_quit),
    Command("wait-ready", "session", "等游戏启动、civ2agent 就绪；协议版本不对时返回 protocol_mismatch。",
            params=(Param("in_game", "boolean", "还要等到进入对局"),
                    Param("wait_s", "integer", "最多等多少秒", default=60)),
            local=local_wait_ready),
    Command("skip-intro", "session", "跳过开场动画，直到主菜单出现。",
            params=(Param("timeout_ms", "integer", "最多等多少毫秒", default=60000),),
            build=request("skip_intro")),
)
BY_NAME = {command.name: command for command in COMMANDS}

RENAMED = {"hello": "status", "snapshot": "state", "menu": "dialog"}


def renamed_hint(argv):
    """A usage message for command names from protocol 1, or None."""
    words = list(argv)
    while words[:1] and words[0] in ("--timeout", "--record"):
        words = words[2:]
    if not words:
        return None
    word = words[0]
    if word in RENAMED:
        return f"{word} 已改名为 {RENAMED[word]}"
    if word == "act":
        name = words[1] if len(words) > 1 else ""
        if name == "research":
            return "act research 已取消：研究项只能在游戏弹出的研究对话框里选，用 respond 回答"
        if name in ("respond", "menu"):
            return "act respond 已改为 respond，--button 改为 --option"
        if name in BY_NAME:
            return f"act {name} 已改为 {name}"
        return "act 已拆成独立命令，用 tools 查看命令列表"
    return None


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise UsageError(message)


def add_globals(parser, suppress):
    default = argparse.SUPPRESS if suppress else None
    parser.add_argument("--timeout", type=float, default=default, help="等待连上游戏的秒数")
    parser.add_argument("--record", default=default, help="把请求和响应追加到这个 jsonl 文件（调试用）")


def add_param(parser, param):
    if param.kind == "boolean":
        parser.add_argument(param.flag, dest=param.name, action="store_true", help=param.help)
        return
    if param.kind == "strings":
        parser.add_argument(param.flag, dest=param.name, action="append", help=param.help)
        return
    parser.add_argument(param.flag, dest=param.name, type=int if param.kind == "integer" else str,
                        required=param.required, choices=param.choices or None, default=param.default,
                        help=param.help)


def build_parser():
    parser = Parser(prog="civ2ctl.py", description="文明 II agent 命令行。每次调用输出一行 JSON。")
    add_globals(parser, suppress=False)
    sub = parser.add_subparsers(dest="cmd", metavar="命令", parser_class=Parser)
    sub.required = True
    for command in COMMANDS:
        p = sub.add_parser(command.name, help=command.help, description=command.help)
        add_globals(p, suppress=True)
        for param in command.params:
            add_param(p, param)
    p = sub.add_parser("tools", help="输出全部命令的 JSON Schema，可直接作为 agent 的工具定义。")
    add_globals(p, suppress=True)
    p = sub.add_parser("call", help="用 JSON 参数运行一个命令，如 call move '{\"unit\":3,\"x\":4,\"y\":2}'；参数为 - 时从 stdin 读。")
    add_globals(p, suppress=True)
    p.add_argument("tool")
    p.add_argument("arguments", nargs="?", default="{}")
    p = sub.add_parser("replay")  # debugging: re-send a --record file, one JSON line per request
    add_globals(p, suppress=True)
    p.add_argument("file")
    return parser


def manifest():
    tools = []
    for command in COMMANDS:
        properties, required = {}, []
        for param in command.params:
            if not param.tool:
                continue
            prop = {"type": param.kind, "description": param.help}
            if param.kind == "strings":
                prop = {"type": "array", "items": {"type": "string"}, "description": param.help}
            if param.choices:
                prop["enum"] = list(param.choices)
            if param.default is not None:
                prop["default"] = param.default
            properties[param.name] = prop
            if param.required:
                required.append(param.name)
        tools.append({
            "name": command.name,
            "kind": command.kind,
            "description": command.help,
            "input_schema": {"type": "object", "properties": properties, "required": required,
                             "additionalProperties": False},
        })
    return {"ok": True, "protocol": PROTOCOL, "usage": "civ2ctl.py call <name> '<JSON arguments>'",
            "exit_codes": {"0": "ok", "1": "游戏拒绝或失败", "2": "参数错误", "3": "连不上游戏或超时"},
            "tools": tools}


def call_args(name, text):
    """Arguments for `call`, checked against the same table as the flags."""
    command = BY_NAME.get(name)
    if not command:
        raise UsageError(f"没有命令 {name}，用 tools 查看命令列表")
    if text == "-":
        text = sys.stdin.read()
    try:
        given = json.loads(text) if text.strip() else {}
    except ValueError as exc:
        raise UsageError(f"参数不是合法 JSON：{exc}")
    if not isinstance(given, dict):
        raise UsageError("参数必须是 JSON 对象")
    params = {p.name: p for p in command.params if p.tool}
    unknown = sorted(set(given) - set(params))
    if unknown:
        raise UsageError(f"{name} 没有参数 {', '.join(unknown)}")
    values = {}
    for param in command.params:
        value = given.get(param.name, param.default if param.tool else None)
        if param.kind == "boolean" and value is None:
            value = False
        if param.kind == "strings" and value is not None:
            if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
                raise UsageError(f"参数 {param.name} 应为字符串数组")
            value = value or None
        elif value is not None:
            expected = {"integer": int, "string": str, "boolean": bool}[param.kind]
            if isinstance(value, bool) and expected is not bool or not isinstance(value, expected):
                raise UsageError(f"参数 {param.name} 应为 {param.kind}")
            if param.choices and value not in param.choices:
                raise UsageError(f"参数 {param.name} 只能是 {', '.join(param.choices)}")
        elif param.required:
            raise UsageError(f"缺少参数 {param.name}")
        values[param.name] = value
    return command, argparse.Namespace(**values)


def run(command, args, connect_s):
    """Run one command; returns (request, response)."""
    if command.local:
        return command.local(args, connect_s)
    req = command.build(command, args)
    if command.answers_dialog:
        response = choose(req, connect_s=connect_s)
    else:
        response = transact(req, connect_s=connect_s)
    if command.after:
        response = command.after(args, response)
    out = getattr(args, "out", None)
    if out:
        with open(out, "w", encoding="utf-8") as handle:
            json.dump(response, handle, ensure_ascii=False, indent=2)
    return req, response


def wait_ready(in_game, wait_s):
    deadline = time.time() + wait_s
    last = None
    while True:
        response = transact({"op": "hello"}, connect_s=min(5.0, max(0.5, deadline - time.time())))
        if response.get("error") != "not_connected":
            if response.get("protocol") != PROTOCOL:
                return fail("protocol_mismatch",
                            f"游戏里的 civ2agent 是协议 {response.get('protocol')}，命令行是协议 {PROTOCOL}，重新编译并注入 DLL",
                            protocol=response.get("protocol"))
            if not response.get("ok"):
                return response
            if not in_game or response.get("in_game"):
                return response
        last = response
        if time.time() >= deadline:
            break
        time.sleep(0.5)
    if last and last.get("error") == "not_connected":
        return last
    return fail("timeout", "等到时限也没有进入对局" if in_game else "等到时限游戏也没有就绪", last=last)


def replay(path, record, connect_s):
    """Re-send the requests of a --record file. Prints one line per request."""
    code = EXIT_OK
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            raw = raw.strip()
            if not raw:
                continue
            item = json.loads(raw)
            req = item.get("request", item)
            if req.get("op") == "act" and req.get("name") == "respond":
                response = choose(req, connect_s=connect_s)
            else:
                response = transact(req, connect_s=connect_s)
            if emit(response, record, req) != EXIT_OK:
                code = EXIT_REFUSED
    return code


def emit(response, record=None, req=None, name=None):
    print(json.dumps(response, ensure_ascii=False))
    if record:
        with open(record, "a", encoding="utf-8") as handle:
            handle.write(json.dumps({"request": req, "response": response}, ensure_ascii=False) + "\n")
    if name:
        log_session(name, req, response)
    return exit_code(response)


def main(argv):
    name = None
    try:
        hint = renamed_hint(argv)
        if hint:
            raise UsageError(hint)
        args = build_parser().parse_args(argv)
        record, timeout = args.record, args.timeout
        if args.cmd == "tools":
            return emit(manifest())
        if args.cmd == "replay":
            return replay(args.file, record, timeout or 10.0)
        if args.cmd == "call":
            command, args = call_args(args.tool, args.arguments)
        else:
            command = BY_NAME[args.cmd]
        name = command.name
        req, response = run(command, args, timeout or command.connect_s)
        return emit(response, record, req, name)
    except UsageError as exc:
        return emit(fail("usage", str(exc)))
    except Exception as exc:  # report, never a bare traceback
        return emit(fail("cli_fault", f"{type(exc).__name__}: {exc}"), name=name)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
