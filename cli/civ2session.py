#!/usr/bin/env python3
"""Agent-owned games of Civilization II, each in its own folder.

A game folder (games/<id>/, not in git) keeps everything about one game: the
user's request and the settings chosen at the start, the agent's notes, every
command it ran, a line per turn, state snapshots and the saves. Any game can be
resumed from its folder.

    civ2session.py new --spec spec.json [--title T] [--request TEXT]
    civ2session.py new --request TEXT          # the agent writes spec.json, then: setup
    civ2session.py setup DIR
    civ2session.py checkpoint                  # once a turn, by the agent
    civ2session.py resume DIR
    civ2session.py close [--status paused|ended] [--keep-running]
    civ2session.py status [DIR]
    civ2session.py list

Like civ2ctl.py, every call prints one JSON line with "ok". Everything in the
game is done through civ2ctl's player actions; the settings are chosen in the
game's own start dialogs and saving goes through its menu, so the screen shows
it all. The user's files in the game folder (saves, CIV2.DAT, HALLFAME.DAT)
are backed up before a game starts and put back when it closes.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

_spec = importlib.util.spec_from_file_location("civ2ctl", Path(__file__).resolve().with_name("civ2ctl.py"))
ctl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ctl)

AGENT_ROOT = ctl.AGENT_ROOT
GAME_ROOT = ctl.GAME_ROOT
LAUNCHER = AGENT_ROOT / "build" / "bin" / "civ2agent-launch.exe"
PLAYBOOK = AGENT_ROOT / "runner" / "playbook.md"
DAT_FILES = ("CIV2.DAT", "HALLFAME.DAT")
KEEP_RECENT = 10  # turn saves kept besides every tenth turn
SNAPSHOT_EVERY = 10
fail = ctl.fail


class SessionError(Exception):
    def __init__(self, error, reason, **extra):
        super().__init__(reason)
        self.response = fail(error, reason, **extra)


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%S")


def write_json(path, value):
    """Whole-file writes only, so a reader never sees half a file."""
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def append_jsonl(path, value):
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(json.dumps(value, ensure_ascii=False) + "\n")


def read_jsonl(path):
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    out = []
    for line in lines:
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --- folders and the lock -------------------------------------------------

def session_dir(text):
    path = Path(text).resolve()
    if not (path / "session.json").is_file():
        raise SessionError("usage", f"{path} 不是游戏局目录（没有 session.json）")
    return path


def load(path):
    return json.loads((path / "session.json").read_text(encoding="utf-8"))


def save(path, session):
    write_json(path / "session.json", session)


def lock_path():
    return ctl.games_dir() / ".active"


def take_lock(path, session):
    active = ctl.active_session()
    if active and Path(active["dir"]).resolve() != path:
        raise SessionError("illegal", f"游戏局 {active.get('id')} 还在进行，先 close")
    lock_path().parent.mkdir(parents=True, exist_ok=True)
    write_json(lock_path(), {"id": session["id"], "dir": str(path), "since": now()})


def require_active():
    active = ctl.active_session()
    if not active:
        raise SessionError("illegal", "没有进行中的游戏局；用 new 或 resume")
    path = Path(active["dir"]).resolve()
    if not (path / "session.json").is_file():
        raise SessionError("game_fault", f"锁指向的目录不是游戏局：{path}")
    return path, load(path)


def new_id(slug):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    slug = re.sub(r"[^a-z0-9-]+", "-", (slug or "").lower()).strip("-")[:32]
    return f"{stamp}-{slug}" if slug else stamp


# --- the game process -------------------------------------------------------

def call(payload, connect_s=10.0):
    return ctl.transact(payload, connect_s=connect_s)


def launch():
    """Start civ2.exe through the launcher (it returns once the DLL is in) and
    wait for the main menu."""
    if not LAUNCHER.is_file():
        raise SessionError("game_fault", f"没有 {LAUNCHER}；先编译 civ2agent")
    proc = subprocess.run([str(LAUNCHER)], cwd=str(GAME_ROOT), capture_output=True, timeout=180)
    if proc.returncode != 0:
        raise SessionError("game_fault", "启动器失败：" + proc.stdout.decode("mbcs", errors="replace")[:200])
    ready = ctl.wait_ready(False, 120)
    if not ready.get("ok"):
        raise SessionError(ready.get("error", "timeout"), ready.get("reason", "游戏没有就绪"), ready=ready)
    skipped = call({"op": "skip_intro", "timeout_ms": 60000}, connect_s=10)
    if not skipped.get("ok"):
        raise SessionError(skipped.get("error", "timeout"), "没有跳过开场动画", skip=skipped)


def ensure_menu():
    """The game running and on its main menu: launched when it is not running.
    Refuses when a match is going on (someone else's game)."""
    status = call({"op": "hello"}, connect_s=2)
    if status.get("error") == "not_connected":
        launch()
        status = call({"op": "hello"}, connect_s=10)
    if not status.get("ok"):
        raise SessionError(status.get("error", "game_fault"), status.get("reason", "游戏状态不对"), status=status)
    if status.get("in_game"):
        raise SessionError("illegal", "游戏里正在进行另一局；先保存并离开那一局")
    if not ctl.to_main_menu(30):
        raise SessionError("timeout", "没有回到游戏主菜单", dialog=ctl.current_dialog())


# --- the start dialogs ------------------------------------------------------
# A spec names what to pick in each start dialog (Game.txt @SIZEOFMAP,
# @DIFFICULTY, ...). Dialogs are recognised by title; rows are picked by the
# text the game shows. What the spec leaves out keeps the game's default.

ROW_RULES = {  # dialog title -> (spec key, {value: text the row starts with})
    "世界大小": ("map_size", {"small": "小", "medium": "中", "large": "大"}),
    "游戏难度": ("difficulty", {"chieftain": "酋长", "warlord": "军阀", "prince": "王子", "king": "国王",
                            "emperor": "皇帝", "deity": "神级"}),
    "竞争程度": ("civs", {3: "三个文明", 4: "四个文明", 5: "五个文明", 6: "六个文明", 7: "七个文明"}),
    "蛮族活动力": ("barbarians", {"villages": "只在村中", "roaming": "四处漫游", "restless": "永无宁日",
                             "raging": "猛烈出击"}),
    "游戏规则": ("rules", {"standard": "标准规则", "custom": "自定规则"}),
    "选择性别": ("gender", {"male": "男性", "female": "女性"}),
    "自定世界: 土地面积": ("world.land", {"small": "小", "normal": "中", "large": "大"}),
    "自定世界: 土地形态": ("world.form", {"archipelago": "岛屿型", "varied": "变化型", "continents": "大陆型"}),
    "自定世界: 气候形态": ("world.climate", {"arid": "乾燥", "normal": "一般", "wet": "潮湿"}),
    "自定世界: 温度设定": ("world.temperature", {"cool": "凉快", "temperate": "一般", "warm": "温暖"}),
    "自定世界: 地质年代": ("world.age", {"3": "三十亿", "4": "四十亿", "5": "五十亿"}),
}
RANDOM = "随机决定"  # the button on the competition, barbarian and world dialogs
SPEC_KEYS = {"start", "map_size", "difficulty", "civs", "barbarians", "rules", "gender", "tribe", "leader",
             "city_style", "world", "title", "id"}
WORLD_KEYS = {"land", "form", "climate", "temperature", "age"}


def spec_value(spec, key):
    if key.startswith("world."):
        return (spec.get("world") or {}).get(key[6:])
    return spec.get(key)


def check_spec(spec):
    """Names of keys and values, before the game is touched."""
    if not isinstance(spec, dict):
        raise SessionError("usage", "spec 必须是 JSON 对象")
    unknown = sorted(set(spec) - SPEC_KEYS)
    if unknown:
        raise SessionError("usage", f"spec 没有这些键：{', '.join(unknown)}")
    if spec.get("start", "new") not in ("new", "custom"):
        raise SessionError("usage", "start 只能是 new（开始新游戏）或 custom（自定世界）")
    world = spec.get("world") or {}
    if not isinstance(world, dict) or set(world) - WORLD_KEYS:
        raise SessionError("usage", f"world 只能有 {', '.join(sorted(WORLD_KEYS))}")
    if world and spec.get("start") != "custom":
        raise SessionError("usage", "world 只在 start 为 custom 时有用")
    size = spec.get("map_size")
    if isinstance(size, dict):
        if set(size) != {"width", "height"} or not all(isinstance(v, int) for v in size.values()):
            raise SessionError("usage", "自定地图尺寸写成 {\"width\": 60, \"height\": 80}")
    for title, (key, values) in ROW_RULES.items():
        value = spec_value(spec, key)
        if value is None or (key == "map_size" and isinstance(value, dict)):
            continue
        if value != "random" and value not in values:
            raise SessionError("usage", f"{key} 只能是 {', '.join(map(str, values))}" +
                               ("、random" if title in ("竞争程度", "蛮族活动力") or key.startswith("world.") else ""))
    for key in ("tribe", "leader", "city_style"):
        if spec.get(key) is not None and not isinstance(spec[key], str):
            raise SessionError("usage", f"{key} 应为字符串")


STANDARD_BUTTONS = ("确定", "取消", "说明", "自定", RANDOM)


def is_row(dialog, option):
    """Rows carry the game's record id; a dialog answered with the mouse has no
    ids, and there everything but the standard buttons is a row."""
    if dialog.get("via") == "game":
        return "id" in option
    return option.get("text") not in STANDARD_BUTTONS


def rows(dialog):
    """The dialog's selectable rows, not its buttons."""
    return [o for o in dialog.get("options") or [] if is_row(dialog, o) and not o.get("disabled")]


def button(dialog, text):
    return next((o for o in dialog.get("options") or [] if not is_row(dialog, o) and o.get("text") == text), None)


def answer(option, edits=None, why=""):
    req = {"op": "act", "name": "respond", "option": option["index"]}
    if edits:
        req["edits"] = edits
    return req, why or option.get("text")


def default_row(dialog):
    found = rows(dialog)
    default = dialog.get("default", -1)
    return next((o for o in found if o["index"] == default), found[0] if found else None)


def pick(dialog, spec):
    """(request, what was chosen) for one start dialog, or (None, reason) when
    the agent has to answer it itself."""
    title = (dialog.get("title") or "").strip()
    ok = button(dialog, "确定")
    if any((o.get("text") or "").strip() == "开始新游戏" for o in rows(dialog)):
        want = "自定世界" if spec.get("start") == "custom" else "开始新游戏"
        row = next((o for o in rows(dialog) if (o.get("text") or "").strip() == want), None)
        return answer(row) if row else (None, f"主菜单上没有 {want}")
    if title == "地图尺寸":
        size = spec.get("map_size")
        if not isinstance(size, dict) or not ok:
            return None, "自定地图尺寸需要 map_size: {width, height}"
        return answer(ok, [str(size["width"]), str(size["height"])], f"{size['width']}x{size['height']}")
    if title in ROW_RULES:
        key, values = ROW_RULES[title]
        value = spec_value(spec, key)
        if key == "map_size" and isinstance(value, dict):
            custom = button(dialog, "自定")
            return answer(custom) if custom else (None, "世界大小对话框没有「自定」")
        if value == "random":
            random = button(dialog, RANDOM)
            return answer(random) if random else (None, f"{title} 没有「{RANDOM}」")
        if value is not None:
            row = next((o for o in rows(dialog) if (o.get("text") or "").startswith(values[value])), None)
            return answer(row) if row else (None, f"{title} 里没有 {values[value]}")
        row = default_row(dialog)
        return answer(row, why=f"{row['text']}（默认）") if row else (None, f"{title} 没有可选的行")
    if title == "选择您的种族":
        tribe = spec.get("tribe")
        if tribe is None:
            row = default_row(dialog)
            return answer(row, why=f"{row['text']}（默认）") if row else (None, "种族列表是空的")
        found = rows(dialog)
        exact = [o for o in found if o.get("text") in (tribe, tribe + "人")]
        near = exact or [o for o in found if tribe in (o.get("text") or "")]
        if len(near) == 1:
            return answer(near[0])
        return None, f"种族 {tribe} 对不上：可选 {'、'.join(o['text'] for o in found)}"
    if title.startswith("请输入您的姓名") and dialog.get("edits") and ok:
        leader = spec.get("leader")
        return answer(ok, [leader] if leader else None, leader or f"{dialog['edits'][0]['text']}（默认）")
    if title == "选择城市风格":
        style = spec.get("city_style")
        row = next((o for o in rows(dialog) if style and style in (o.get("text") or "")), None) or default_row(dialog)
        if row:
            return answer(row)
        return answer(ok) if ok else (None, "城市风格对话框没有可选项")
    # Anything else with nothing to choose (a warning, a message): OK.
    if ok and not rows(dialog):
        return answer(ok, why=f"{title}：确定")
    return None, f"开局时出现了 spec 没有写到的对话框「{title}」"


def drive_setup(spec, steps, deadline_s=420):
    """Answer the start dialogs until the match is on; each answer goes to steps."""
    deadline = time.time() + deadline_s
    last, repeats = None, 0
    while time.time() < deadline:
        status = call({"op": "hello"}, connect_s=5)
        if status.get("error") in ("not_connected", "timeout"):
            time.sleep(1)  # building the world keeps the game thread busy
            continue
        if status.get("in_game"):
            return
        dialog = ctl.current_dialog()
        if not dialog:
            time.sleep(0.5)
            continue
        key = ctl.dialog_key(dialog)
        repeats = repeats + 1 if key == last else 0
        last = key
        if repeats >= 3:
            raise SessionError("game_fault", f"开局对话框「{dialog.get('title')}」答了没有反应", dialog=dialog)
        req, chosen = pick(dialog, spec)
        if req is None:
            raise SessionError("modal_open", chosen, dialog=dialog)
        response = ctl.choose(req)
        if not response.get("ok") and response.get("error") not in ("timeout", "not_connected"):
            raise SessionError(response.get("error", "game_fault"), response.get("reason", "回答开局对话框失败"),
                               dialog=dialog, response=response)
        steps.append({"title": dialog.get("title"), "chosen": chosen})
    raise SessionError("timeout", "开局对话框没有答完")


def after_start(steps, settle_s=30):
    """The messages the game shows as the match begins get OK, their text kept
    in steps; the first dialog that asks for a choice (research) is the
    agent's, and is returned."""
    deadline = time.time() + settle_s
    quiet = 0
    while time.time() < deadline and quiet < 3:
        dialog = ctl.current_dialog()
        if not dialog:
            quiet += 1
            time.sleep(0.7)
            continue
        quiet = 0
        ok = button(dialog, "确定")
        if rows(dialog) or not ok or dialog.get("edits") or dialog.get("via") != "game":
            return dialog
        steps.append({"title": dialog.get("title"), "text": dialog.get("text"), "chosen": "确定"})
        if not ctl.choose(answer(ok)[0]).get("ok"):
            return ctl.current_dialog()
    return ctl.current_dialog()


def autosave_prefix(leader):
    """How the game's autosave name starts: the leader name's first two bytes
    (one Chinese character or two letters), as 0x488937 builds it."""
    return (leader or "").encode("mbcs", errors="replace")[:2].decode("mbcs", errors="replace")


# --- the user's files -------------------------------------------------------

def root_saves():
    return sorted(p for p in GAME_ROOT.glob("*.SAV") if p.is_file())


def backup(path):
    """Copy what a game can change in the game folder into backup/, with hashes."""
    folder = path / "backup"
    folder.mkdir(exist_ok=True)
    files = {}
    for src in [GAME_ROOT / name for name in DAT_FILES] + root_saves():
        if src.is_file():
            shutil.copy2(src, folder / src.name)
            files[src.name] = {"sha256": sha256(src), "bytes": src.stat().st_size}
    write_json(folder / "manifest.json", {"time": now(), "root": str(GAME_ROOT), "files": files})
    return sorted(files)


def reconcile(path, session, restore_dat):
    """Put the user's files back as they were before the game. A root save this
    game changed is kept in saves/ first; new root saves named like this game's
    autosaves move into saves/; other new files are only reported."""
    folder = path / "backup"
    try:
        files = json.loads((folder / "manifest.json").read_text(encoding="utf-8")).get("files") or {}
    except (OSError, ValueError):
        return {"note": "没有备份清单，什么也没动"}
    restored, moved, left = [], [], []
    for name, info in files.items():
        live = GAME_ROOT / name
        if (name in DAT_FILES and not restore_dat) or (live.is_file() and sha256(live) == info["sha256"]):
            continue
        if live.is_file() and name not in DAT_FILES:
            shutil.copy2(live, path / "saves" / f"root-{name}")
        shutil.copy2(folder / name, live)
        restored.append(name)
    prefix = session.get("prefix")
    for live in root_saves():
        if live.name in files:
            continue
        if prefix and live.name.startswith(prefix + "_"):
            shutil.move(str(live), str(path / "saves" / live.name))
            moved.append(live.name)
        else:
            left.append(live.name)
    return {"restored": restored, "moved": moved, "left": left}


# --- checkpoints ------------------------------------------------------------

def turn_line(state):
    civ = state.get("civ") or {}
    return {"time": now(), "turn": state.get("turn"), "year": state.get("year"), "gold": civ.get("gold"),
            "tax": civ.get("tax_rate"), "science": civ.get("science_rate"), "lux": civ.get("lux_rate"),
            "government": civ.get("government"), "research": civ.get("research_name"),
            "techs": len(civ.get("techs") or []), "units": len(state.get("units") or []),
            "cities": [{"name": c.get("name"), "size": c.get("size"), "building": c.get("building_name")}
                       for c in state.get("cities") or []],
            "foreign_cities": len(state.get("foreign_cities") or []), "diplomacy": state.get("diplomacy")}


def prune(path, session):
    """Turn saves this game made: keep the last KEEP_RECENT and every tenth turn."""
    mine = sorted(n for n in session.get("saves", []) if re.fullmatch(r"T\d{4}\.SAV", n))
    keep = set(mine[-KEEP_RECENT:]) | {n for n in mine if int(n[1:5]) % 10 == 0}
    removed = [n for n in mine if n not in keep]
    for name in removed:
        (path / "saves" / name).unlink(missing_ok=True)
    session["saves"] = [n for n in session.get("saves", []) if n not in removed]
    return removed


def checkpoint(path=None, session=None):
    """Save this turn into saves/ through the game's menu, one line in
    turns.jsonl, a snapshot every tenth turn. The first time, the save is also
    loaded back: the game then writes its own autosaves into saves/ too."""
    if path is None:
        path, session = require_active()
    status = call({"op": "hello"}, connect_s=5)
    if not status.get("ok"):
        raise SessionError(status.get("error", "not_connected"), status.get("reason", "连不上游戏"), status=status)
    if not status.get("in_game"):
        raise SessionError("not_in_game", "游戏不在对局中", status=status)
    if status.get("dialog_open") or status.get("city_window") or status.get("tax_window"):
        raise SessionError("modal_open", "先回答对话框、关上城市或税率窗口，再存盘点", dialog=ctl.current_dialog(),
                           city_window=status.get("city_window"), tax_window=status.get("tax_window"))
    state = call({"op": "snapshot"}, connect_s=10)
    if not state.get("in_game"):
        raise SessionError("not_in_game", "读不到局面", state=state)
    turn = state["turn"]
    result = {"ok": True, "turn": turn, "year": state.get("year")}
    if not session.get("redirected"):
        start = path / "saves" / "start.SAV"
        saved = ctl.save_game(start, overwrite=True)
        if not saved.get("ok"):
            raise SessionError(saved.get("error", "game_fault"), "开局存档失败：" + saved.get("reason", ""), save=saved)
        loaded = ctl.load_game(start)
        if not loaded.get("ok"):
            raise SessionError(loaded.get("error", "game_fault"), "读回开局存档失败：" + loaded.get("reason", ""),
                               load=loaded)
        session["redirected"] = True
        session["root_files"] = reconcile(path, session, restore_dat=False)
        result["redirected"] = True
        result["root_files"] = session["root_files"]
    target = path / "saves" / f"T{turn:04d}.SAV"
    result["saved"] = None
    if not target.exists():
        saved = ctl.save_game(target)
        if not saved.get("ok"):
            raise SessionError(saved.get("error", "game_fault"), "存档失败：" + saved.get("reason", ""), save=saved)
        session.setdefault("saves", []).append(target.name)
        result["saved"] = target.name
    turns = path / "turns.jsonl"
    if not any(t.get("turn") == turn for t in read_jsonl(turns)[-3:]):
        append_jsonl(turns, turn_line(state))
    snap = path / "snapshots" / f"T{turn:04d}.json"
    if (turn == 1 or turn % SNAPSHOT_EVERY == 0) and not snap.exists():
        snap.parent.mkdir(exist_ok=True)
        write_json(snap, state)
    result["pruned"] = prune(path, session)
    session.update(turn=turn, year=state.get("year"), latest_save=target.name, updated=now())
    save(path, session)
    return result


# --- lifecycle --------------------------------------------------------------

NOTES_SEED = """# 笔记

这是 agent 自己的长期记忆。每段开始时先读它，每段结束前更新它：下一段的 agent 只能从这里知道之前想过什么。

## 战略

## 当前计划

## 教训
"""


def year_text(year):
    if year is None:
        return "-"
    return f"公元前 {-year} 年" if year < 0 else f"公元 {year} 年"


def write_brief(path, session):
    lines = [f"# {session['title']}", "", f"游戏局 {session['id']}，{session['created']} 建立。", ""]
    if session.get("request"):
        lines += ["## 用户的要求", "", session["request"], ""]
    if session.get("spec"):
        lines += ["## 开局设置", "", "```json", json.dumps(session["spec"], ensure_ascii=False, indent=2), "```", ""]
    (path / "brief.md").write_text("\n".join(lines), encoding="utf-8")


def read_spec(text):
    if text == "-":
        text = sys.stdin.read()
    elif not text.lstrip().startswith("{"):
        text = Path(text).read_text(encoding="utf-8")
    try:
        return json.loads(text)
    except ValueError as exc:
        raise SessionError("usage", f"spec 不是合法 JSON：{exc}")


def identity():
    state = call({"op": "snapshot"}, connect_s=10)
    if not state.get("in_game"):
        raise SessionError("not_in_game", "不在对局中", state=state)
    civ = state.get("civ") or {}
    return {"civ": civ.get("name"), "leader": civ.get("leader"), "title": civ.get("title"),
            "human": state.get("human"), "difficulty": state.get("difficulty")}


def cmd_new(args):
    if args.spec is None and not args.request:
        raise SessionError("usage", "new 需要 --spec 或 --request")
    spec = read_spec(args.spec) if args.spec is not None else None
    if spec is not None:
        check_spec(spec)
    active = ctl.active_session()
    if active:
        raise SessionError("illegal", f"游戏局 {active.get('id')} 还在进行，先 close")
    title = args.title or (spec or {}).get("title") or (args.request or "")[:30] or "文明 II"
    sid = new_id(args.id or (spec or {}).get("id"))
    path = ctl.games_dir() / sid
    (path / "saves").mkdir(parents=True)
    (path / "agent").mkdir()
    (path / "agent" / "notes.md").write_text(NOTES_SEED, encoding="utf-8")
    session = {"id": sid, "title": title, "created": now(), "updated": now(), "status": "setup",
               "request": args.request, "spec": spec, "setup": [], "identity": None, "prefix": None,
               "redirected": False, "saves": [], "turn": None, "year": None, "latest_save": None,
               "segments": 0, "exe_hash": None}
    save(path, session)
    write_brief(path, session)
    if spec is None:
        return {"ok": True, "dir": str(path), "id": sid, "status": "setup",
                "next": f"按用户的要求写 {path / 'spec.json'}，再运行 civ2session.py setup {path}"}
    write_json(path / "spec.json", spec)
    return setup(path)


def setup(path):
    """Start the game by spec.json. Run again after answering a start dialog
    the spec did not cover: it goes on from the dialog on screen."""
    session = load(path)
    if session["status"] not in ("setup", "starting", "failed"):
        raise SessionError("illegal", f"这局的状态是 {session['status']}，不能再开局；继续玩用 resume")
    spec_file = path / "spec.json"
    if not spec_file.is_file():
        raise SessionError("usage", f"没有 {spec_file}")
    spec = read_spec(str(spec_file))
    check_spec(spec)
    active = ctl.active_session()
    own = bool(active and Path(active["dir"]).resolve() == path)
    if active and not own:
        raise SessionError("illegal", f"游戏局 {active.get('id')} 还在进行，先 close")
    status = call({"op": "hello"}, connect_s=2)
    if not own and status.get("ok") and status.get("in_game"):
        raise SessionError("illegal", "游戏里正在进行另一局；先保存并离开那一局")
    if status.get("error") in ("version_mismatch", "unsupported_limits", "protocol_mismatch"):
        raise SessionError(status["error"], status.get("reason", "游戏和 civ2agent 不匹配"), status=status)
    steps = list(session.get("setup") or []) if own else []
    if not own:
        take_lock(path, session)
    session.update(spec=spec, status="starting", updated=now())
    save(path, session)
    write_brief(path, session)
    try:
        files = None if own else backup(path)
        if not own or status.get("error") == "not_connected":
            ensure_menu()
            status = call({"op": "hello"}, connect_s=10)
        if not status.get("in_game"):
            drive_setup(spec, steps)
        waiting = after_start(steps)
        who = identity()
        session.update(setup=steps, identity=who, prefix=autosave_prefix(who.get("leader")), status="active",
                       exe_hash=call({"op": "hello"}).get("exe_hash"), updated=now())
        save(path, session)
    except Exception as exc:
        reason = exc.response.get("reason") if isinstance(exc, SessionError) else f"{type(exc).__name__}: {exc}"
        session.update(status="failed", setup=steps, error=reason, updated=now())
        save(path, session)
        if isinstance(exc, SessionError):
            exc.response.update(dir=str(path), status="failed",
                                next=f"用 civ2ctl.py respond 回答屏幕上的对话框，再运行 setup {path} 接着开局；"
                                     "放弃这局用 civ2session.py close")
        raise
    result = {"ok": True, "dir": str(path), "id": session["id"], "status": "active", "identity": who,
              "setup": steps, "backup": files}
    if waiting:
        result["dialog"] = waiting
        result["next"] = "回答这个对话框，再运行 civ2session.py checkpoint"
    else:
        result["checkpoint"] = checkpoint(path, session)
    return result


def cmd_setup(args):
    return setup(session_dir(args.dir))


def same_game(session):
    who, want = identity(), session.get("identity") or {}
    return all(who.get(k) == want.get(k) for k in ("civ", "leader", "human"))


def load_latest(path):
    """The newest save of this game that the game reads back."""
    saves = sorted((p for p in (path / "saves").glob("*.SAV") if not p.name.startswith("root-")),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    last = None
    for candidate in saves[:3]:
        last = ctl.load_game(candidate)
        if last.get("ok"):
            return last
        ctl.to_main_menu(20)
    raise SessionError("game_fault", "这局没有读得进来的存档", last=last)


def cmd_resume(args):
    path = session_dir(args.dir)
    session = load(path)
    if session["status"] in ("setup", "starting", "failed"):
        raise SessionError("illegal", "这局还没开局成功；用 setup")
    if session["status"] == "ended":
        raise SessionError("illegal", "这局已经结束")
    active = ctl.active_session()
    if active and Path(active["dir"]).resolve() != path:
        raise SessionError("illegal", f"游戏局 {active.get('id')} 还在进行，先 close")
    status = call({"op": "hello"}, connect_s=2)
    attached = bool(status.get("ok") and status.get("in_game"))
    if attached and not same_game(session):
        raise SessionError("illegal", "游戏里正在进行另一局；先保存并离开那一局")
    files = None if active else backup(path)  # still locked: the files were never put back
    take_lock(path, session)
    session.update(status="active", updated=now())
    save(path, session)
    loaded = None
    if not attached:
        ensure_menu()
        loaded = load_latest(path)
        if not same_game(session):
            raise SessionError("game_fault", "读进来的不是这一局", identity=identity())
    state = call({"op": "snapshot"}, connect_s=10)
    session.update(turn=state.get("turn"), year=state.get("year"), updated=now())
    save(path, session)
    return {"ok": True, "dir": str(path), "id": session["id"], "turn": state.get("turn"), "year": state.get("year"),
            "attached": attached, "loaded": (loaded or {}).get("file"), "backup": files, "dialog": state.get("dialog")}


def cmd_checkpoint(args):
    path, session = require_active()
    return dict(checkpoint(path, session), dir=str(path))


def cmd_close(args):
    active = ctl.active_session()
    if not active:
        raise SessionError("illegal", "没有进行中的游戏局")
    path = Path(active["dir"]).resolve()
    session = load(path)
    result = {"ok": True, "id": session["id"], "dir": str(path)}
    if ctl.game_running():
        status = call({"op": "hello"}, connect_s=3)
        if status.get("ok") and status.get("in_game") and session.get("identity") and not same_game(session):
            raise SessionError("illegal", "游戏里正在进行的不是这一局，不会存它也不会退出它；先离开那一局再 close")
        if status.get("ok") and status.get("in_game"):
            # Messages with only OK (a beginner's tip after loading) are read
            # and closed, as a player leaving would; a choice stops close.
            dismissed = []
            after_start(dismissed, settle_s=10)
            if dismissed:
                result["dismissed"] = dismissed
            try:
                result["checkpoint"] = checkpoint(path, session)
                session = load(path)
            except SessionError as exc:
                result["warning"] = "这回合没有存上：" + exc.response.get("reason", "")
        if not args.keep_running:
            result["quit"] = ctl.quit_game(exit_program=True)
    if not args.keep_running and ctl.game_running():
        raise SessionError("illegal", "游戏没有退出，用户的文件没有恢复，锁也还在；回答游戏里的对话框后再 close",
                           result=result)
    result["files"] = reconcile(path, session, restore_dat=not args.keep_running)
    lock_path().unlink(missing_ok=True)
    # A game that never got going stays as it was, so setup can start it later.
    started = session["status"] not in ("setup", "starting", "failed")
    session.update(status=args.status if started else "failed", closed=now(), updated=now())
    save(path, session)
    return result


def cmd_status(args):
    active = ctl.active_session()
    if args.dir:
        path = session_dir(args.dir)
    elif active:
        path = Path(active["dir"]).resolve()
    else:
        raise SessionError("illegal", "没有进行中的游戏局；给出目录")
    is_active = bool(active and Path(active["dir"]).resolve() == path)
    result = {"ok": True, "dir": str(path), "active": is_active, "session": load(path),
              "recent_turns": read_jsonl(path / "turns.jsonl")[-5:]}
    if is_active:
        hello = call({"op": "hello"}, connect_s=2)
        result["game"] = {"running": hello.get("error") != "not_connected", "in_game": hello.get("in_game"),
                          "dialog_open": hello.get("dialog_open")}
    return result


def cmd_list(args):
    active = ctl.active_session()
    root = ctl.games_dir()
    games = []
    for folder in sorted(root.iterdir()) if root.is_dir() else []:
        try:
            s = load(folder)
        except (OSError, ValueError):
            continue
        games.append({"id": s.get("id"), "title": s.get("title"), "status": s.get("status"), "turn": s.get("turn"),
                      "year": s.get("year"), "civ": (s.get("identity") or {}).get("civ"), "dir": str(folder),
                      "active": bool(active and Path(active["dir"]).resolve() == folder.resolve())})
    return {"ok": True, "games": games}


def cmd_context(args):
    """The agent's folder for one stretch of play (runner): the playbook, the
    brief, and context.md with where the game stands and this stretch's goal."""
    path = session_dir(args.dir)
    session = load(path)
    agent = path / "agent"
    agent.mkdir(exist_ok=True)
    if PLAYBOOK.is_file():
        shutil.copy2(PLAYBOOK, agent / "playbook.md")
    shutil.copy2(path / "brief.md", agent / "brief.md")
    if not (agent / "notes.md").exists():
        (agent / "notes.md").write_text(NOTES_SEED, encoding="utf-8")
    who = session.get("identity") or {}
    lines = ["# 这一段", "", args.goal or "继续玩。", "", "# 这一局", "",
             f"- 目录：{path}", f"- 状态：{session['status']}",
             f"- 文明：{who.get('civ') or '-'}，领袖：{who.get('leader') or '-'}，难度：{who.get('difficulty', '-')}",
             f"- 现在：第 {session.get('turn') or '-'} 回合，{year_text(session.get('year'))}",
             f"- 最近的存档：{session.get('latest_save') or '-'}", ""]
    if session["status"] == "failed" and session.get("error"):
        lines[-1:-1] = [f"- 开局停在：{session['error']}"]
    recent = read_jsonl(path / "turns.jsonl")[-8:]
    if recent:
        lines += ["# 最近几回合", "", "| 回合 | 年份 | 金币 | 研究 | 单位 | 城市 |", "|---|---|---|---|---|---|"]
        for t in recent:
            lines.append(f"| {t.get('turn')} | {year_text(t.get('year'))} | {t.get('gold')} | {t.get('research') or '-'} "
                         f"| {t.get('units')} | {len(t.get('cities') or [])} |")
        lines.append("")
    runs = read_jsonl(path / "runs.jsonl")[-3:]
    if runs:
        lines += ["# 前几段", ""]
        for r in runs:
            lines.append(f"- 第 {r.get('segment')} 段（{r.get('target')}）：{r.get('status')}，"
                         f"回合 {r.get('turn_before')} → {r.get('turn_after')}。{(r.get('summary') or '').strip()[:200]}")
        lines.append("")
    (agent / "context.md").write_text("\n".join(lines), encoding="utf-8")
    return {"ok": True, "dir": str(path), "workspace": str(agent), "status": session["status"],
            "turn": session.get("turn"), "year": session.get("year"), "segments": session.get("segments", 0)}


def cmd_record_run(args):
    path = session_dir(args.dir)
    record = json.loads(args.json)
    if not isinstance(record, dict):
        raise SessionError("usage", "record-run 的参数必须是 JSON 对象")
    session = load(path)
    record.setdefault("time", now())
    append_jsonl(path / "runs.jsonl", record)
    session["segments"] = session.get("segments", 0) + 1
    session["last_run"] = {k: record.get(k) for k in ("segment", "target", "status", "turn_after")}
    session["updated"] = now()
    save(path, session)
    return {"ok": True, "segments": session["segments"]}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise SessionError("usage", message)


def build_parser():
    parser = Parser(prog="civ2session.py", description="agent 自己的文明 II 游戏局：每局一个目录，可以接着玩。")
    sub = parser.add_subparsers(dest="cmd", metavar="命令", parser_class=Parser)
    sub.required = True
    p = sub.add_parser("new", help="建一局：目录、要求和开局设置；给了 spec 就启动游戏并开局")
    p.add_argument("--spec", help="开局设置：JSON 文件、JSON 文字或 -（stdin）")
    p.add_argument("--request", help="用户的原话，记进 brief.md")
    p.add_argument("--title", help="这局的名字")
    p.add_argument("--id", help="目录名里的英文标识，如 romans-king")
    p.set_defaults(run=cmd_new)
    p = sub.add_parser("setup", help="按目录里的 spec.json 启动游戏并开局")
    p.add_argument("dir")
    p.set_defaults(run=cmd_setup)
    p = sub.add_parser("checkpoint", help="存盘点：存这一回合、记一行、隔几回合存一份局面；每回合一次")
    p.set_defaults(run=cmd_checkpoint)
    p = sub.add_parser("resume", help="从目录接着玩：启动游戏，读最新的存档，核对是这一局")
    p.add_argument("dir")
    p.set_defaults(run=cmd_resume)
    p = sub.add_parser("close", help="收尾：存盘点，离开并退出游戏，恢复用户的文件，解锁")
    p.add_argument("--status", choices=("paused", "ended"), default="paused")
    p.add_argument("--keep-running", action="store_true", help="不退出游戏（用户的 CIV2.DAT、HALLFAME.DAT 不恢复）")
    p.set_defaults(run=cmd_close)
    p = sub.add_parser("status", help="一局的状态；不给目录时是进行中的那局")
    p.add_argument("dir", nargs="?")
    p.set_defaults(run=cmd_status)
    p = sub.add_parser("list", help="所有游戏局")
    p.set_defaults(run=cmd_list)
    p = sub.add_parser("context", help="（runner 用）准备 agent 目录和 context.md")
    p.add_argument("dir")
    p.add_argument("--goal")
    p.set_defaults(run=cmd_context)
    p = sub.add_parser("record-run", help="（runner 用）记一段 agent 运行的结果")
    p.add_argument("dir")
    p.add_argument("json")
    p.set_defaults(run=cmd_record_run)
    return parser


def log_folder(args):
    """The game a session command acts on, found before it runs (close drops the lock)."""
    active = ctl.active_session()
    if active:
        return Path(active["dir"])
    return Path(args.dir) if getattr(args, "dir", None) else None


def main(argv):
    args = folder = None
    try:
        args = build_parser().parse_args(argv)
        folder = log_folder(args)
        response = args.run(args)
    except SessionError as exc:
        response = exc.response
    except Exception as exc:  # report, never a bare traceback
        response = fail("cli_fault", f"{type(exc).__name__}: {exc}")
    print(json.dumps(response, ensure_ascii=False))
    folder = folder or (Path(response["dir"]) if response.get("dir") else None)
    if args is not None and args.cmd not in ("list", "status", "context") and folder and \
            (folder / "session.json").is_file():
        try:
            append_jsonl(folder / "log.jsonl", {"time": now(), "command": f"session {args.cmd}", "response": response})
        except OSError:
            pass  # the log must not change the command's outcome
    return ctl.exit_code(response)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
