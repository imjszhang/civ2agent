#!/usr/bin/env python3
"""Irrigate with a human settler, then advance turns until the terrain bit flips.

Requires a single-player game already loaded and civ2agent injected.
Exits 0 with a skip message when no game is loaded, so a menu-only smoke run
does not fail. Set CIV2_AGENT_REQUIRE_GAME=1 to make that a failure.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

CLI = Path(__file__).resolve().parents[1] / "cli" / "civ2ctl.py"
IRRIGATION = 0x04


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


def neighbor_dirs():
    return ((1, -1), (2, 0), (1, 1), (0, 2), (-1, 1), (-2, 0), (-1, -1), (0, -2))


def main():
    try:
        hello = run("hello")
    except SystemExit as exc:
        print("skip:", exc)
        return 1 if os.environ.get("CIV2_AGENT_REQUIRE_GAME") == "1" else 0
    if not hello.get("in_game"):
        print("skip: not in a game")
        return 1 if os.environ.get("CIV2_AGENT_REQUIRE_GAME") == "1" else 0
    state = run("snapshot", timeout=60)
    if not state.get("ok"):
        print(json.dumps(state, ensure_ascii=False))
        return 1
    settler = next((u for u in state["units"] if u.get("civ") == state["human"] and u.get("role") == 5), None)
    if not settler:
        print("skip: no human settler")
        return 1 if os.environ.get("CIV2_AGENT_REQUIRE_GAME") == "1" else 0
    tiles = {(t["x"], t["y"]): t for t in state["tiles"]}
    target = None
    for dx, dy in neighbor_dirs():
        pos = (settler["x"] + dx, settler["y"] + dy)
        tile = tiles.get(pos)
        if not tile:
            continue
        terrain = tile["t"] & 0x0F
        if terrain == 0x0A:
            continue
        if tile["f"] & IRRIGATION:
            continue
        target = pos
        break
    if target is None:
        print("skip: no dry land next to the settler")
        return 0
    ordered = run("act", "order", "--unit", str(settler["index"]), "--order", "irrigate")
    print("order", json.dumps(ordered, ensure_ascii=False))
    if not ordered.get("ok"):
        return 1
    if ordered.get("orders") != 6:
        print("orders field did not become irrigate")
        return 1
    for turn in range(8):
        ended = run("end-turn", "--timeout-ms", "180000", timeout=200)
        print("end", json.dumps({k: ended.get(k) for k in ("ok", "error", "turn", "year")}, ensure_ascii=False))
        if not ended.get("ok"):
            return 1
        state = run("snapshot", timeout=60)
        tile = next((t for t in state.get("tiles", []) if t["x"] == target[0] and t["y"] == target[1]), None)
        if tile and tile["f"] & IRRIGATION:
            print("irrigated", target, "after", turn + 1, "turns")
            return 0
    print("irrigation bit still clear at", target)
    return 1


if __name__ == "__main__":
    sys.exit(main())
