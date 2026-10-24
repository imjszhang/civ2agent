#!/usr/bin/env python3
"""Play the first turns of a new game through the CLI and check each command.

Requires civ2agent injected and the game at its main menu (or already in a
match). Starts a new game with default settings and the leader name 测试员,
so it writes 测_自动.SAV and 测_自动2.SAV: back up *.SAV, *.HOT and *.DAT
first, quit through 离开游戏 afterwards (tests/game_menu.py), restore
CIV2.DAT, and remove the autosaves it created. Prints one line per check and
a summary; exits 1 if any check failed.
"""

import json
import subprocess
import sys
import time
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

CLI = Path(__file__).resolve().parents[1] / "cli" / "civ2ctl.py"
CITY_NAME = "测试城"
LEADER_NAME = "测试员"  # autosaves go to 测_自动.SAV / 测_自动2.SAV
failures = []


def cli(*args, timeout=240):
    proc = subprocess.run([sys.executable, str(CLI), *args], capture_output=True, text=True,
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


def note(text):
    print("NOTE " + text)


def option(dialog, *texts):
    """The first option whose text is one of texts, else None."""
    for o in (dialog or {}).get("options") or []:
        if o.get("text") in texts and not o.get("disabled"):
            return o
    return None


def respond_ok(dialog):
    """Answer a dialog the plain way: 确定 if it has one, else its default or first option."""
    choice = option(dialog, "确定", "是")
    if choice is None:
        options = [o for o in (dialog or {}).get("options") or [] if not o.get("disabled")]
        default = (dialog or {}).get("default", -1)
        choice = next((o for o in options if o["index"] == default), options[0] if options else None)
    if choice is None:
        return {"ok": False, "error": "no_option"}
    return cli("respond", "--option", str(choice["index"]))


def drain_dialogs(limit=12):
    """Answer whatever the game shows until no dialog is left."""
    for _ in range(limit):
        dialog = cli("dialog").get("dialog")
        if not dialog:
            return True
        r = respond_ok(dialog)
        note(f"answered {dialog.get('title')!r}: ok={r.get('ok')} via={r.get('via')}")
        if not r.get("ok"):
            return False
    return False


def dump(response):
    return json.dumps(response, ensure_ascii=False)[:300]


def start_game():
    if cli("status").get("in_game"):
        return True
    dialog = cli("dialog").get("dialog")
    if not check("main menu showing", option(dialog, "开始新游戏") is not None):
        return False
    cli("respond", "--text", "开始新游戏")
    for _ in range(15):
        dialog = cli("dialog").get("dialog")
        if not dialog:
            break  # world generation keeps the game thread busy
        if dialog.get("edits") and option(dialog, "确定"):
            # The leader name picks the autosave file (first character +
            # _自动.SAV); a default name like 尤利乌斯·恺撒 would overwrite a real save.
            r = cli("respond", "--text", "确定", "--edit", LEADER_NAME)
        else:
            r = respond_ok(dialog)
        if not check(f"new game: {dialog.get('title')}", r.get("ok"), "" if r.get("ok") else dump(r)):
            return False
    ready = cli("wait-ready", "--in-game", "--wait-s", "120", timeout=200)
    if not check("match started", bool(ready.get("ok") and ready.get("in_game")), dump(ready)):
        return False
    drain_dialogs()
    return True


def check_state(expect_units=True):
    s = cli("state")
    if not check("state", bool(s.get("ok") and s.get("in_game")), s.get("error", "")):
        return None
    civ = s.get("civ") or {}
    check("state: no debug data", not ({"civs", "treaties", "gold", "year_computed"} & set(s)))
    check("state: civ block", all(k in civ for k in ("gold", "tax_rate", "science_rate", "lux_rate", "government",
                                                      "research", "beakers", "research_cost", "techs")), dump(civ))
    check("state: year is negative BC", s.get("year", 0) < 0, str(s.get("year")))
    units = s.get("units") or []
    check("state: own units", (bool(units) or not expect_units) and
          all("moves_left" in u and "veteran" in u for u in units), str(len(units)))
    tiles = s.get("tiles") or []
    check("state: tiles only on real squares", bool(tiles) and all((t["x"] + t["y"]) % 2 == 0 for t in tiles))
    check("state: tiles unique, no v", len({(t["x"], t["y"]) for t in tiles}) == len(tiles) and
          all("v" not in t for t in tiles), str(len(tiles)))
    foreign = s.get("foreign_units") or []
    check("state: foreign units carry no internals",
          all(not ({"index", "id", "moves_left", "orders", "home"} & set(u)) for u in foreign), str(len(foreign)))
    note(f"turn {s.get('turn')} year {s.get('year')} units {len(units)} foreign {len(foreign)} tiles {len(tiles)} "
         f"cities {len(s.get('cities') or [])} diplomacy {s.get('diplomacy')} city_window {s.get('city_window')}")
    return s


def wait_city_window_closed():
    for _ in range(10):
        s = cli("state")
        if s.get("ok") and s.get("city_window") is None:
            return True
        time.sleep(0.3)
    return False


def found_city(state):
    settler = next((u for u in state.get("units") or [] if u.get("role") == 5), None)
    if not check("a settler to found with", settler is not None):
        return None
    r = cli("found-city", "--unit", str(settler["index"]))
    dialog = r.get("dialog")
    if not check("found-city stops at the naming dialog", r.get("error") == "modal_open" and dialog is not None,
                 dump(r)):
        return None
    note(f"naming dialog: {dialog.get('title')!r} edits={dialog.get('edits')} "
         f"options={[o['text'] for o in dialog.get('options') or []]}")
    ok_option = option(dialog, "确定") or (dialog.get("options") or [{}])[0]
    r = cli("respond", "--option", str(ok_option.get("index", 0)), "--edit", CITY_NAME)
    check("respond --edit names the city", r.get("ok"), dump(r))
    s = cli("state")
    city = next((c for c in s.get("cities") or [] if c.get("name") == CITY_NAME), None)
    check(f"city {CITY_NAME} exists", city is not None, str([c.get("name") for c in s.get("cities") or []]))
    note(f"after founding: city_window={s.get('city_window')} dialog={(s.get('dialog') or {}).get('title')!r}")
    return city or next(iter(s.get("cities") or []), None)


def produce(city):
    index = str(city["index"])
    look = cli("produce", "--city", index)
    options = look.get("options") or []
    check("produce (look) lists the game's options", bool(look.get("ok") and options and look.get("changed") is False),
          dump(look))
    current = look.get("building")
    other = next((o for o in options if o["building"] != current and not o.get("disabled")), None)
    if not check("a different item to build", other is not None):
        return
    r = cli("produce", "--city", index, "--item-id", str(other["id"]))
    check("produce --item-id", bool(r.get("ok") and r.get("building") == other["building"] and r.get("changed")),
          dump(r))
    s = cli("state")
    now = next((c for c in s.get("cities") or [] if c.get("index") == city["index"]), {})
    check("state shows the new production", now.get("building") == other["building"], str(now.get("building")))
    back = next((o for o in options if o["building"] == current), None)
    if back:
        r = cli("produce", "--city", index, "--item", back["name"])
        check("produce --item (by name) switches back", bool(r.get("ok") and r.get("building") == current), dump(r))
    r = cli("produce", "--city", index, "--item", "不存在的东西")
    check("produce refuses an item the list lacks", r.get("error") == "illegal" and bool(r.get("options")), dump(r))
    s = cli("state")
    if s.get("city_window"):
        note(f"city window still open: {s.get('city_window')}")
        r = cli("close-city")
        check("close-city", r.get("ok"), dump(r))
    check("city window closed", wait_city_window_closed())


def rates(state):
    civ = state.get("civ") or {}
    original = (civ.get("tax_rate"), civ.get("science_rate"))
    if civ.get("government") == 1:  # despotism allows at most 60%
        r = cli("rates", "--tax", "7", "--science", "3")
        check("rates refuses more than the government allows", r.get("error") == "illegal" and "max_rate" in r,
              dump(r))
    target = (5, 5) if original != (5, 5) else (4, 6)
    r = cli("rates", "--tax", str(target[0]), "--science", str(target[1]))
    check(f"rates sets {target}", r.get("ok"), dump(r))
    now = cli("state").get("civ") or {}
    check("state shows the new rates", (now.get("tax_rate"), now.get("science_rate")) == target, dump(now))
    if None not in original:
        r = cli("rates", "--tax", str(original[0]), "--science", str(original[1]))
        check("rates restores the old rates", r.get("ok"), dump(r))


def category_change(city):
    """Unit to improvement with shields stored: the game asks to confirm the loss. Recorded, not judged."""
    if not city.get("shields"):
        note(f"no shields in {city.get('name')}, skipping the category change")
        return
    look = cli("produce", "--city", str(city["index"]))
    improvement = next((o for o in look.get("options") or [] if o["building"] < 0), None)
    if not improvement:
        note("no improvement to switch to")
        return
    before = (city.get("building"), city.get("shields"))
    r = cli("produce", "--city", str(city["index"]), "--item-id", str(improvement["id"]))
    dialog = r.get("dialog") or {}
    note(f"category change {before} -> {improvement['name']}: {r.get('error') or 'ok'} "
         f"dialog={dialog.get('title')!r} text={dialog.get('text')!r} options={[o['text'] for o in dialog.get('options') or []]}")
    if r.get("error") == "modal_open" and dialog:
        answer = option(dialog, "取消") or (dialog.get("options") or [{}])[-1]
        cli("respond", "--option", str(answer.get("index", 0)))
        s = cli("state")
        now = next((c for c in s.get("cities") or [] if c.get("index") == city["index"]), {})
        note(f"after answering {answer.get('text')!r}: building={now.get('building')} shields={now.get('shields')}")
        if s.get("city_window"):
            check("close-city after the confirmation", cli("close-city").get("ok"))
    elif r.get("ok"):
        note(f"no confirmation: shields {before[1]} -> {r.get('shields')}")
    # Switching back is a category change too; answer its confirmation the same way.
    r = cli("produce", "--city", str(city["index"]), "--item-id", str(before[0]))
    if r.get("error") == "modal_open" and r.get("dialog"):
        respond_ok(r["dialog"])
    if cli("dialog").get("city_window"):
        cli("close-city")


def end_turns(count):
    for _ in range(count):
        before = cli("state").get("turn")
        r = cli("end-turn", "--timeout-ms", "120000", timeout=200)
        for _ in range(12):
            if r.get("error") != "modal_open":
                break
            if r.get("dialog"):
                answered = respond_ok(r["dialog"])
                note(f"end-turn dialog {r['dialog'].get('title')!r}: ok={answered.get('ok')}")
            elif r.get("city_window"):
                answered = cli("close-city")
                note(f"end-turn city window {r['city_window']}: close ok={answered.get('ok')}")
            if r.get("turn_ended"):
                drain_dialogs()
                r = {"ok": True}
                break
            r = cli("end-turn", "--timeout-ms", "120000", timeout=200)
        after = cli("state").get("turn")
        # With nothing left to move the game ends turns by itself, so it can be more than one.
        check("end-turn reaches a later turn", bool(r.get("ok") and (after or 0) > (before or 0)),
              f"{before} -> {after} {dump(r)}")


def main():
    if cli("status").get("error") == "not_connected":
        print("skip: the game is not running")
        return 0
    ready = cli("wait-ready")
    if not check("protocol 2", bool(ready.get("ok") and ready.get("protocol") == 2), dump(ready)):
        return 1
    if not start_game():
        return 1
    state = check_state()
    if state is None:
        return 1
    # Rates first: once the settler founds the city there is nothing left to
    # move, and closing the city window lets the game end the turn.
    rates(state)
    city = next(iter(state.get("cities") or []), None) or found_city(state)
    drain_dialogs()
    if city:
        produce(city)
    drain_dialogs()
    end_turns(2)
    s = check_state(expect_units=False)
    if city and s:
        # Now from a closed city window: the command opens and closes it.
        produce(city)
        category_change(next((c for c in s.get("cities") or [] if c.get("index") == city["index"]), city))
    note(f"events: {dump(cli('events'))}")
    print(f"{len(failures)} failed: {failures}" if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
