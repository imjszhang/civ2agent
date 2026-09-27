#!/usr/bin/env python3
"""Thin client for the civ2agent named pipe. Agent tools call this program."""

import argparse
import ctypes
import json
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")

PIPE = r"\\.\pipe\civ2agent"

try:
    ctypes.windll.user32.SetProcessDPIAware()
except Exception:
    pass


def transact(payload, timeout_s=30):
    data = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        try:
            with open(PIPE, "r+b", buffering=0) as pipe:
                pipe.write(data)
                buf = b""
                while b"\n" not in buf:
                    chunk = pipe.read(65536)
                    if not chunk:
                        break
                    buf += chunk
                    if time.time() > deadline:
                        break
            line = buf.split(b"\n", 1)[0].decode("utf-8", errors="replace")
            if not line:
                raise RuntimeError("空响应")
            return json.loads(line)
        except FileNotFoundError as exc:
            last = exc
            time.sleep(0.25)
        except OSError as exc:
            last = exc
            time.sleep(0.25)
    raise SystemExit(f"连接 {PIPE} 失败: {last}")


def click_screen(x, y, hwnd, repeats=2):
    user32 = ctypes.windll.user32
    if hwnd:
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.15)
    for _ in range(max(1, repeats)):
        user32.SetCursorPos(int(x), int(y))
        time.sleep(0.05)
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.05)
        user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(0.3)


def choose(request, timeout_s=30):
    """Pick a dialog option. The DLL selects list rows itself; for controls it
    returns screen coordinates, because the game ignores posted mouse input."""
    response = transact(request, timeout_s=timeout_s)
    click = response.pop("click", None)
    if response.get("ok") and click:
        click_screen(click["x"], click["y"], click.get("hwnd"), repeats=click.get("repeats", 1))
        response["via"] = "mouse"
    time.sleep(0.4)
    return response


def emit(response, record, request):
    text = json.dumps(response, ensure_ascii=False)
    print(text)
    if record:
        with open(record, "a", encoding="utf-8") as handle:
            handle.write(json.dumps({"request": request, "response": response}, ensure_ascii=False) + "\n")
    return 0 if response.get("ok") else 1


def build_parser():
    parser = argparse.ArgumentParser(description="文明 II agent 控制台")
    parser.add_argument("--record", help="把请求和响应追加到 jsonl")
    parser.add_argument("--timeout", type=int, default=30, help="等待管道的秒数")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("hello")
    sub.add_parser("events")
    sub.add_parser("menu")
    snap = sub.add_parser("snapshot")
    snap.add_argument("--debug", action="store_true")
    snap.add_argument("-o", dest="out")

    end = sub.add_parser("end-turn")
    end.add_argument("--timeout-ms", type=int, default=120000)

    skip = sub.add_parser("skip-intro")
    skip.add_argument("--timeout-ms", type=int, default=60000)

    ready = sub.add_parser("wait-ready")
    ready.add_argument("--timeout", type=int, default=60)
    ready.add_argument("--in-game", action="store_true")

    act = sub.add_parser("act")
    act.add_argument("name", choices=[
        "activate", "move", "goto", "order", "found-city", "produce", "research", "rates", "respond", "menu",
    ])
    act.add_argument("--unit", type=int)
    act.add_argument("--city", type=int)
    act.add_argument("--x", type=int)
    act.add_argument("--y", type=int)
    act.add_argument("--tech", type=int)
    act.add_argument("--tax", type=int)
    act.add_argument("--science", type=int)
    act.add_argument("--button", type=int)
    act.add_argument("--kind")
    act.add_argument("--id", dest="item_id", type=int)
    act.add_argument("--order")
    act.add_argument("--text")

    replay = sub.add_parser("replay")
    replay.add_argument("file")
    return parser


def request_from_args(args):
    if args.cmd == "hello":
        return {"op": "hello"}
    if args.cmd == "menu":
        return {"op": "menu"}
    if args.cmd == "events":
        return {"op": "events"}
    if args.cmd == "snapshot":
        body = {"op": "snapshot", "debug": bool(args.debug)}
        return body
    if args.cmd == "end-turn":
        return {"op": "end_turn", "timeout_ms": args.timeout_ms}
    if args.cmd == "skip-intro":
        return {"op": "skip_intro", "timeout_ms": args.timeout_ms}
    if args.cmd == "act":
        name = "found_city" if args.name == "found-city" else args.name
        body = {"op": "act", "name": name}
        for key, value in (
            ("unit", args.unit),
            ("city", args.city),
            ("x", args.x),
            ("y", args.y),
            ("tax", args.tax),
            ("science", args.science),
            ("button", args.button),
            ("kind", args.kind),
            ("order", args.order),
            ("text", args.text),
        ):
            if value is not None:
                body[key] = value
        if args.name == "produce" and args.item_id is not None:
            body["item"] = args.item_id
        if args.name == "research":
            chosen = args.tech if args.tech is not None else args.item_id
            if chosen is not None:
                body["tech"] = chosen
        return body
    return None


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.cmd == "wait-ready":
        deadline = time.time() + args.timeout
        last = None
        while time.time() < deadline:
            try:
                response = transact({"op": "hello"}, timeout_s=5)
            except SystemExit as exc:
                last = str(exc)
                time.sleep(0.5)
                continue
            last = response
            if response.get("protocol") != 1:
                time.sleep(0.5)
                continue
            if args.in_game and not response.get("in_game"):
                time.sleep(0.5)
                continue
            return emit(response, args.record, {"op": "hello"})
        print(json.dumps({"ok": False, "error": "timeout", "reason": str(last)}, ensure_ascii=False))
        return 1
    if args.cmd == "replay":
        code = 0
        with open(args.file, encoding="utf-8") as handle:
            for raw in handle:
                raw = raw.strip()
                if not raw:
                    continue
                item = json.loads(raw)
                request = item.get("request", item)
                response = transact(request, timeout_s=max(args.timeout, int(request.get("timeout_ms", 0)) / 1000 + 5))
                if emit(response, args.record, request) != 0:
                    code = 1
        return code

    if args.cmd == "act" and args.name == "menu":
        request = {"op": "act", "name": "menu"}
        if args.text:
            request["text"] = args.text
        elif args.button is not None:
            request["button"] = args.button
        response = choose(request, timeout_s=args.timeout)
        return emit(response, args.record, request)

    request = request_from_args(args)
    wait = args.timeout
    if args.cmd == "end-turn":
        wait = max(wait, args.timeout_ms / 1000 + 5)
    if args.cmd == "skip-intro":
        wait = max(wait, args.timeout_ms / 1000 + 5)
    response = transact(request, timeout_s=wait)
    if args.cmd == "snapshot" and args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            json.dump(response, handle, ensure_ascii=False, indent=2)
    return emit(response, args.record, request)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
