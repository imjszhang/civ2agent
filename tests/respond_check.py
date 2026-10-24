#!/usr/bin/env python3
"""Pick non-default rows with act respond, then back out to the main menu.

Requires civ2agent injected and the main menu on screen. These dialogs are
answered through the game's own handlers (via=game), so the mouse is not used.
Choosing 女性 and then 埃及人 (a later column of the tribe list) must offer the
name 克利奥帕特拉 in the name box; any other name means a row other than the
requested one was confirmed. Never starts a game, so no save is written.
Exits 0 with a skip message when the main menu is not showing. Set
CIV2_AGENT_REQUIRE_GAME=1 to make that a failure.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

CLI = Path(__file__).resolve().parents[1] / "cli" / "civ2ctl.py"
MAIN_MENU = "开始新游戏"


def run(*args, timeout=60):
    proc = subprocess.run(
        [sys.executable, str(CLI), "--timeout", "5", *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )
    if not proc.stdout.strip():
        raise SystemExit(proc.stderr.strip() or "管道没有响应")
    return json.loads(proc.stdout)


def options(dialog):
    return [o["text"] for o in (dialog or {}).get("options") or []]


def edits(dialog):
    return [e["text"] for e in (dialog or {}).get("edits") or []]


def respond(text):
    r = run("act", "respond", "--text", text)
    dialog = r.get("dialog") or {}
    print(f"respond {text!r}: ok={r.get('ok')} via={r.get('via')} changed={r.get('changed')} -> {dialog.get('title')!r}")
    if not r.get("ok") or not r.get("changed"):
        raise SystemExit(json.dumps(r, ensure_ascii=False))
    return dialog


def skip(reason):
    print("skip:", reason)
    return 1 if os.environ.get("CIV2_AGENT_REQUIRE_GAME") == "1" else 0


def main():
    try:
        menu = run("menu")
    except SystemExit as exc:
        return skip(exc)
    if not menu.get("ok") or MAIN_MENU not in options(menu.get("menu")):
        return skip("main menu not showing")
    dialog = respond(MAIN_MENU)
    for _ in range(8):
        if "女性" in options(dialog):
            break
        dialog = respond("确定")
    else:
        print("never reached the gender dialog")
        return 1
    respond("女性")
    dialog = respond("埃及人")
    name = edits(dialog)
    print("name offered:", name[:1])
    passed = name[:1] == ["克利奥帕特拉"]
    for _ in range(12):
        if MAIN_MENU in options(dialog):
            break
        dialog = respond("取消")
    print("PASS" if passed else "FAIL: a different row was confirmed")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
