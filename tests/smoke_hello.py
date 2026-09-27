#!/usr/bin/env python3
"""Launch via Civ2UIA, inject civ2agent, and require a hello response."""

import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAUNCH = ROOT / "civ2agent" / "build" / "bin" / "civ2agent-launch.exe"
CLI = ROOT / "civ2agent" / "cli" / "civ2ctl.py"


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
        deadline = time.time() + 20
        response = None
        while time.time() < deadline:
            run = subprocess.run(
                [sys.executable, str(CLI), "hello"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
            )
            if run.stdout.strip():
                response = json.loads(run.stdout)
                break
            time.sleep(0.5)
        if response is None:
            print("no hello")
            return 1
        print(json.dumps(response, ensure_ascii=False, indent=2))
        if response.get("protocol") != 1:
            return 1
        if "exe_hash" not in response or "uia_loaded" not in response:
            return 1
        snap = subprocess.run(
            [sys.executable, str(CLI), "snapshot"],
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
                snapshot.get("gold"),
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
