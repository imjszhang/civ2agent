#!/usr/bin/env python3
"""Launch via Civ2UIA, inject civ2agent, and require a hello response."""

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

AGENT = Path(__file__).resolve().parents[1]
LAUNCH = AGENT / "build" / "bin" / "civ2agent-launch.exe"
CLI = AGENT / "cli" / "civ2ctl.py"
_spec = importlib.util.spec_from_file_location("civ2ctl", CLI)
_ctl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_ctl)
try:
    ROOT = _ctl.game_root()
except _ctl.GamePathError as exc:
    raise SystemExit(str(exc)) from exc


def civ2_running():
    out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq civ2.exe"], text=True, errors="replace")
    return "civ2.exe" in out.lower()


def main():
    if not LAUNCH.exists():
        print("missing", LAUNCH)
        return 1
    already = civ2_running()
    if already:
        print("civ2.exe already running; injecting only")
    proc = subprocess.Popen([str(LAUNCH), "--game", str(ROOT)], cwd=str(ROOT))
    try:
        code = proc.wait(timeout=90)
        if code != 0:
            print("launcher exit", code)
            return code
        run = subprocess.run(
            [sys.executable, str(CLI), "wait-ready", "--wait-s", "20"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=40,
        )
        response = json.loads(run.stdout)
        print(json.dumps(response, ensure_ascii=False, indent=2))
        if not response.get("ok") or response.get("protocol") != 3:
            return 1
        if "exe_hash" not in response or "uia_loaded" not in response:
            return 1
        snap = subprocess.run(
            [sys.executable, str(CLI), "state"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=20,
        )
        print(snap.stdout)
        snapshot = json.loads(snap.stdout)
        if snapshot.get("ok"):
            print(
                "in game turn",
                snapshot.get("turn"),
                "gold",
                (snapshot.get("civ") or {}).get("gold"),
                "cities",
                len(snapshot.get("cities") or []),
            )
        elif snapshot.get("error") not in ("not_in_game", "version_mismatch", "unsupported_limits"):
            print("unexpected snapshot error")
            return 1
        return 0
    finally:
        if not already:
            subprocess.run(["taskkill", "/F", "/IM", "civ2.exe"], check=False, capture_output=True)
            subprocess.run(["taskkill", "/F", "/IM", "Civ2UIALauncher.exe"], check=False, capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
