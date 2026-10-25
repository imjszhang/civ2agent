#!/usr/bin/env python3
"""Run one agent-owned game through its whole life and check each step.

new (small map, leader 测试员) -> two turns with plain answers and a checkpoint
each -> close -> resume -> close. Checks that the game's own autosaves land in
the game folder's saves/, and that every file in the game root (saves,
CIV2.DAT, HALLFAME.DAT) is byte for byte as before. Needs the game NOT running
and no active session; it starts and quits the game itself. Copy *.SAV, *.HOT
and *.DAT somewhere else first anyway. The game folder it makes stays in
civ2agent/games/ for inspection.
"""

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

CLI = Path(__file__).resolve().parents[1] / "cli"
GAME_ROOT = Path(__file__).resolve().parents[2]
SPEC = {"map_size": "small", "difficulty": "chieftain", "civs": 3, "barbarians": "villages", "rules": "standard",
        "gender": "male", "tribe": "罗马", "leader": "测试员", "id": "session-check"}
failures = []


def cli(script, *args, timeout=600):
    proc = subprocess.run([sys.executable, str(CLI / script), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)
    try:
        return json.loads(proc.stdout.splitlines()[0])
    except (IndexError, ValueError):
        return {"ok": False, "error": "no_json", "reason": (proc.stdout + proc.stderr)[:300]}


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (f"  {detail}" if detail else ""))
    if not ok:
        failures.append(name)
    return ok


def root_hashes():
    files = [p for p in GAME_ROOT.iterdir() if p.is_file() and p.suffix.upper() in (".SAV", ".HOT", ".DAT")]
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}


def drain(limit=15):
    """Plain answers: OK on messages, the highlighted row on lists."""
    for _ in range(limit):
        d = cli("civ2ctl.py", "dialog")
        if d.get("city_window"):
            cli("civ2ctl.py", "close-city")
            continue
        dialog = d.get("dialog")
        if not dialog:
            return
        options = [o for o in dialog.get("options") or [] if not o.get("disabled")]
        rows = [o for o in options if "id" in o]
        pick = (next((o for o in rows if o["index"] == dialog.get("default")), rows[0]) if rows else
                next((o for o in options if o["text"] == "确定"), options[0]))
        cli("civ2ctl.py", "respond", "--option", str(pick["index"]))


def play_turn():
    drain()
    point = cli("civ2session.py", "checkpoint")
    check(f"checkpoint turn {point.get('turn')}", point.get("ok"), json.dumps(point, ensure_ascii=False)[:200])
    r = cli("civ2ctl.py", "end-turn")
    for _ in range(10):
        if r.get("error") != "modal_open":
            break
        ended = r.get("turn_ended")
        drain()
        if ended:
            return
        r = cli("civ2ctl.py", "end-turn")
    check("end-turn", r.get("ok"), json.dumps(r, ensure_ascii=False)[:200])


def main():
    if cli("civ2ctl.py", "status", "--timeout", "1").get("error") != "not_connected":
        print("skip: quit the game first; this check starts it")
        return 0
    before = root_hashes()
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as handle:
        json.dump(SPEC, handle, ensure_ascii=False)
    made = cli("civ2session.py", "new", "--spec", handle.name, "--title", "会话检查", timeout=1200)
    Path(handle.name).unlink()
    if not check("new", made.get("ok") and made.get("status") == "active", json.dumps(made, ensure_ascii=False)[:300]):
        return 1
    folder = Path(made["dir"])
    check("identity", (made.get("identity") or {}).get("leader") == "测试员", str(made.get("identity")))
    for _ in range(2):
        play_turn()
    drain()
    cli("civ2session.py", "checkpoint")
    autosaves = sorted(p.name for p in (folder / "saves").glob("测_自动*.SAV"))
    check("autosaves land in saves/", bool(autosaves), str(autosaves))
    closed = cli("civ2session.py", "close")
    check("close", closed.get("ok") and (closed.get("quit") or {}).get("exited"), json.dumps(closed, ensure_ascii=False)[:300])
    check("root files as before", root_hashes() == before)
    resumed = cli("civ2session.py", "resume", str(folder), timeout=600)
    check("resume", resumed.get("ok") and resumed.get("turn") == 3, json.dumps(resumed, ensure_ascii=False)[:300])
    closed = cli("civ2session.py", "close", "--status", "ended")
    check("close again", closed.get("ok"), json.dumps(closed, ensure_ascii=False)[:300])
    check("root files as before at the end", root_hashes() == before)
    print(f"{len(failures)} failed: {failures}" if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
