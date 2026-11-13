#!/usr/bin/env python3
"""Offline tests for cli/civ2session.py: run without the game.

python -m unittest discover -s civ2agent/tests -p "test_*.py"
"""

import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

CLI = Path(__file__).resolve().parents[1] / "cli" / "civ2session.py"
spec = importlib.util.spec_from_file_location("civ2session", CLI)
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)


def dialog(title, rows=(), buttons=("确定",), default=0, edits=None, via="game"):
    """A dialog as the DLL reports it: rows with ids, then buttons."""
    options = [{"index": i, "text": t, "id": 10 + i} for i, t in enumerate(rows)]
    options += [{"index": len(rows) + i, "text": t} for i, t in enumerate(buttons)]
    out = {"title": title, "text": "", "kind": "list" if rows else "controls", "via": via, "options": options}
    if rows:
        out["default"] = default
    if edits is not None:
        out["edits"] = [{"index": i, "text": t} for i, t in enumerate(edits)]
    return out


def run(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = session.main(argv)
    return code, json.loads(out.getvalue().splitlines()[0])


class Spec(unittest.TestCase):
    def test_good_specs_pass(self):
        session.check_spec({})
        session.check_spec({"map_size": "small", "difficulty": "king", "civs": 5, "barbarians": "random",
                            "rules": "standard", "gender": "female", "tribe": "罗马", "leader": "测试员"})
        session.check_spec({"start": "custom", "map_size": {"width": 60, "height": 80},
                            "world": {"land": "large", "age": "random"}})

    def test_bad_specs_are_usage_errors(self):
        for bad in ([], {"colour": "red"}, {"difficulty": "hard"}, {"civs": 8}, {"world": {"land": "small"}},
                    {"start": "scenario"}, {"start": "custom", "world": {"sea": 1}}, {"map_size": {"width": 60}},
                    {"tribe": 3}):
            with self.subTest(spec=bad):
                with self.assertRaises(session.SessionError) as caught:
                    session.check_spec(bad)
                self.assertEqual(caught.exception.response["error"], "usage")


class StartDialogs(unittest.TestCase):
    def choose(self, d, spec):
        req, chosen = session.pick(d, spec)
        return (d["options"][req["option"]]["text"] if req else None), req, chosen

    def test_main_menu(self):
        menu = dialog("文明 II 多人游戏黄金版", ["开始新游戏", "开始预先做好的世界", "自定世界", "读取进度 "], buttons=())
        self.assertEqual(self.choose(menu, {})[0], "开始新游戏")
        self.assertEqual(self.choose(menu, {"start": "custom"})[0], "自定世界")

    def test_rows_by_spec_or_default(self):
        size = dialog("世界大小", ["小(40x50格, 闪电战)", "中(50x80格)", "大(75x120格, 持久战)"], ["自定", "确定"], default=1)
        self.assertEqual(self.choose(size, {"map_size": "large"})[0], "大(75x120格, 持久战)")
        self.assertEqual(self.choose(size, {})[0], "中(50x80格)")
        self.assertEqual(self.choose(size, {"map_size": {"width": 60, "height": 80}})[0], "自定")
        hard = dialog("游戏难度", ["酋长(最容易)", "军阀", "王子", "国王", "皇帝", "神级(最困难)"], ())
        self.assertEqual(self.choose(hard, {"difficulty": "deity"})[0], "神级(最困难)")
        rivals = dialog("竞争程度", ["七个文明", "六个文明", "五个文明", "四个文明", "三个文明"], ["随机决定", "确定"])
        self.assertEqual(self.choose(rivals, {"civs": 4})[0], "四个文明")
        self.assertEqual(self.choose(rivals, {"civs": "random"})[0], "随机决定")

    def test_custom_size_types_both_boxes(self):
        size = dialog("地图尺寸", (), ["确定", "取消"], edits=["50", "80"])
        text, req, chosen = self.choose(size, {"map_size": {"width": 60, "height": 90}})
        self.assertEqual((text, req["edits"], chosen), ("确定", ["60", "90"], "60x90"))

    def test_tribe_and_leader(self):
        tribes = dialog("选择您的种族", ["罗马人", "巴比伦人", "德国人", "埃及人"], ["自定", "确定"])
        self.assertEqual(self.choose(tribes, {"tribe": "罗马"})[0], "罗马人")
        self.assertEqual(self.choose(tribes, {"tribe": "埃及人"})[0], "埃及人")
        req, reason = session.pick(tribes, {"tribe": "火星"})
        self.assertIsNone(req)
        self.assertIn("巴比伦人", reason)
        name = dialog("请输入您的姓名:", (), ["确定", "取消"], edits=["尤利乌斯·恺撒"])
        self.assertEqual(self.choose(name, {"leader": "测试员"})[1]["edits"], ["测试员"])
        self.assertNotIn("edits", self.choose(name, {})[1])

    def test_unknown_choices_go_to_the_agent_and_messages_get_ok(self):
        req, reason = session.pick(dialog("选择第1个对手", ["随机设定", "罗马人"], ()), {})
        self.assertIsNone(req)
        self.assertIn("选择第1个对手", reason)
        self.assertEqual(self.choose(dialog("警告！", (), ["确定"]), {})[0], "确定")

    def test_mouse_dialogs_without_ids(self):
        tribes = dialog("选择您的种族", ["罗马人", "埃及人"], ["自定", "确定"], via="mouse")
        for option in tribes["options"]:
            option.pop("id", None)
        self.assertEqual(self.choose(tribes, {"tribe": "埃及"})[0], "埃及人")


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.game = root / "game"
        self.game.mkdir()
        (self.game / "civ2.exe").write_bytes(b"")
        self.dir = root / "games" / "g1"
        (self.dir / "saves").mkdir(parents=True)
        self.old_game = os.environ.get("CIV2AGENT_GAME")
        os.environ["CIV2AGENT_GAME"] = str(self.game)
        os.environ["CIV2AGENT_GAMES_DIR"] = str(root / "games")

    def tearDown(self):
        if self.old_game is None:
            os.environ.pop("CIV2AGENT_GAME", None)
        else:
            os.environ["CIV2AGENT_GAME"] = self.old_game
        del os.environ["CIV2AGENT_GAMES_DIR"]
        self.tmp.cleanup()

    def test_autosave_prefix_is_two_bytes(self):
        self.assertEqual(session.autosave_prefix("尤利乌斯·恺撒"), "尤")
        self.assertEqual(session.autosave_prefix("Hegemon"), "He")
        self.assertEqual(session.autosave_prefix(None), "")

    def test_backup_then_reconcile_puts_the_user_files_back(self):
        (self.game / "CIV2.DAT").write_bytes(b"prefs")
        (self.game / "尤_自动.SAV").write_bytes(b"user save")
        files = session.backup(self.dir)
        self.assertEqual(files, ["CIV2.DAT", "尤_自动.SAV"])
        (self.game / "尤_自动.SAV").write_bytes(b"agent turn 1")  # the same prefix, overwritten
        (self.game / "CIV2.DAT").write_bytes(b"agent prefs")
        (self.game / "测_自动.SAV").write_bytes(b"agent autosave")
        (self.game / "其它.SAV").write_bytes(b"someone else")
        result = session.reconcile(self.dir, {"prefix": "测"}, restore_dat=False)
        self.assertEqual((self.game / "尤_自动.SAV").read_bytes(), b"user save")
        self.assertEqual((self.dir / "saves" / "root-尤_自动.SAV").read_bytes(), b"agent turn 1")
        self.assertEqual((self.dir / "saves" / "测_自动.SAV").read_bytes(), b"agent autosave")
        self.assertFalse((self.game / "测_自动.SAV").exists())
        self.assertEqual((self.game / "CIV2.DAT").read_bytes(), b"agent prefs")
        self.assertEqual(result, {"restored": ["尤_自动.SAV"], "moved": ["测_自动.SAV"], "left": ["其它.SAV"]})
        session.reconcile(self.dir, {"prefix": "测"}, restore_dat=True)
        self.assertEqual((self.game / "CIV2.DAT").read_bytes(), b"prefs")

    def test_prune_keeps_recent_and_every_tenth(self):
        names = [f"T{t:04d}.SAV" for t in range(1, 26)]
        for name in names + ["start.SAV"]:
            (self.dir / "saves" / name).write_bytes(b"x")
        state = {"saves": names + ["start.SAV"]}
        removed = session.prune(self.dir, state)
        kept = {f"T{t:04d}.SAV" for t in (10, *range(16, 26))} | {"start.SAV"}
        self.assertEqual(set(state["saves"]), kept)
        self.assertEqual(len(removed), 25 - 11)
        self.assertTrue((self.dir / "saves" / "start.SAV").exists())
        self.assertFalse((self.dir / "saves" / "T0001.SAV").exists())


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        os.environ["CIV2AGENT_GAMES_DIR"] = self.tmp.name

    def tearDown(self):
        del os.environ["CIV2AGENT_GAMES_DIR"]
        self.tmp.cleanup()

    def test_new_with_a_request_waits_for_the_spec(self):
        code, out = run(["new", "--request", "用罗马人玩国王难度", "--id", "Romans King"])
        self.assertEqual((code, out["status"]), (0, "setup"))
        folder = Path(out["dir"])
        self.assertTrue(folder.name.endswith("-romans-king"))
        self.assertIn("用罗马人玩国王难度", (folder / "brief.md").read_text(encoding="utf-8"))
        self.assertTrue((folder / "agent" / "notes.md").exists())
        self.assertIsNone(session.ctl.active_session())
        code, out = run(["context", str(folder), "--goal", "写 spec.json 并开局"])
        self.assertEqual(code, 0)
        context = (folder / "agent" / "context.md").read_text(encoding="utf-8")
        self.assertIn("写 spec.json 并开局", context)
        self.assertIn("存档时的局面", context)
        self.assertTrue((folder / "agent" / "playbook.md").exists())
        code, out = run(["record-run", str(folder), json.dumps({"segment": 1, "status": "completed"})])
        self.assertEqual((code, out["segments"]), (0, 1))
        self.assertEqual(json.loads((folder / "session.json").read_text(encoding="utf-8"))["segments"], 1)

    def test_errors_are_json_lines(self):
        for argv, error in ((["new"], "usage"), (["new", "--spec", '{"difficulty": "hard"}'], "usage"),
                            (["checkpoint"], "illegal"), (["close"], "illegal"), (["resume", self.tmp.name], "usage"),
                            (["bogus"], "usage")):
            with self.subTest(argv=argv):
                code, out = run(argv)
                self.assertEqual(out["error"], error)
                self.assertEqual(code, 2 if error == "usage" else 1)

    def test_a_second_game_waits_for_the_first(self):
        Path(self.tmp.name, ".active").write_text(json.dumps({"id": "x", "dir": self.tmp.name}), encoding="utf-8")
        code, out = run(["new", "--request", "再来一局"])
        self.assertEqual((code, out["error"]), (1, "illegal"))
        code, out = run(["list"])
        self.assertEqual((code, out["games"]), (0, []))

    def test_context_without_a_game_uses_the_saved_turn(self):
        code, out = run(["new", "--request", "从存档看局面", "--id", "saved screen"])
        folder = Path(out["dir"])
        line = {"turn": 4, "year": -3920, "gold": 40, "tax": 6, "science": 4, "lux": 0,
                "government": 2, "research": "字母", "techs": 3, "units": 2,
                "cities": [{"name": "罗马", "size": 2, "building": "民兵"}],
                "foreign_cities": 1,
                "diplomacy": [{"civ": 5, "name": "美国人", "status": "peace", "embassy": False}]}
        (folder / "turns.jsonl").write_text(json.dumps(line, ensure_ascii=False) + "\n", encoding="utf-8")
        original = session.call
        session.call = lambda payload, connect_s=10.0: {"ok": False, "error": "not_connected", "reason": "没开"}
        try:
            code, out = run(["context", str(folder), "--goal", "从存档看"])
        finally:
            session.call = original
        self.assertEqual(code, 0)
        text = (folder / "agent" / "context.md").read_text(encoding="utf-8")
        self.assertIn("从存档看", text)
        self.assertIn("存档时的局面", text)
        self.assertIn("罗马", text)
        self.assertIn("民兵", text)
        self.assertIn("60%", text)
        self.assertIn("美国人 和平 无使馆", text)
        self.assertIn("存档行里没有", text)

    def test_context_uses_the_live_screen_without_the_map(self):
        code, out = run(["new", "--request", "看当前画面", "--id", "live screen"])
        folder = Path(out["dir"])
        live = {
            "ok": True, "in_game": True, "turn": 12, "year": 100, "active_unit": 3, "move_unit": 3,
            "civ": {"name": "罗马人", "leader": "测试员", "title": "大帝", "government": 2, "gold": 80,
                    "tax_rate": 5, "science_rate": 5, "lux_rate": 0, "research_name": "货币",
                    "beakers": 10, "research_cost": 40, "techs": [1, 2]},
            "cities": [{"name": "罗马", "size": 3, "food": 17, "shields": 9, "building_name": "战士"}],
            "units": [{"index": 3, "name": "战士", "x": 10, "y": 12, "moves_left": 3, "orders": 2}],
            "foreign_units": [{"civ": 5, "name": "弓箭手", "x": 20, "y": 14}],
            "foreign_cities": [{"owner": 5, "name": "华盛顿", "x": 16, "y": 4, "size": 2}],
            "diplomacy": [{"civ": 5, "name": "美国人", "status": "peace", "embassy": True}],
            "dialog": {"title": "研究", "text": "选一项", "options": [{"text": "货币"}]},
            "city_window": None, "tax_window": None, "notice": None,
            "tiles": [{"x": 0, "y": 0, "t": 1, "f": 0}],
        }

        def fake(payload, connect_s=10.0):
            if payload.get("op") == "hello":
                return {"ok": True, "in_game": True}
            if payload.get("op") == "snapshot":
                return live
            return {"ok": False, "error": "not_connected"}

        original = session.call
        session.call = fake
        try:
            code, out = run(["context", str(folder), "--goal", "接着玩"])
        finally:
            session.call = original
        self.assertEqual(code, 0)
        text = (folder / "agent" / "context.md").read_text(encoding="utf-8")
        self.assertIn("接着玩", text)
        self.assertIn("| 罗马 | 3 | 17 | 9 | 战士 |", text)
        self.assertIn("| 选中 战士 | 10,12 | 1 | 已设防 |", text)
        self.assertIn("10 / 40", text)
        self.assertNotIn("存档时的局面", text)
        self.assertNotIn("tiles", text)

    def test_context_without_a_guide_leaves_strategy_alone(self):
        code, out = run(["new", "--request", "不看攻略", "--id", "no guide"])
        self.assertEqual(code, 0)
        folder = Path(out["dir"])
        strategy = folder / "agent" / "strategy.md"
        strategy.write_text("上次的打法\n", encoding="utf-8")
        (folder / "agent" / "guide.md").write_text("旧攻略\n", encoding="utf-8")
        code, out = run(["context", str(folder), "--goal", "接着玩"])
        self.assertEqual(code, 0)
        self.assertFalse((folder / "agent" / "guide.md").exists())
        self.assertEqual(strategy.read_text(encoding="utf-8"), "上次的打法\n")
        rules = (folder / "agent" / "rule.md").read_text(encoding="utf-8")
        self.assertIn("先看局面再下令", rules)
        (folder / "agent" / "rule.md").write_text("改过\n", encoding="utf-8")
        code, out = run(["context", str(folder), "--goal", "再来"])
        self.assertEqual(code, 0)
        self.assertEqual((folder / "agent" / "rule.md").read_text(encoding="utf-8"), "改过\n")
        self.assertIsNone(json.loads((folder / "session.json").read_text(encoding="utf-8")).get("guide"))

    def test_context_copies_the_whole_guide_and_does_not_write_strategy(self):
        code, out = run(["new", "--request", "按金元帝国", "--id", "jin yuan", "--guide", "jin-yuan"])
        self.assertEqual((code, out["status"]), (0, "setup"))
        folder = Path(out["dir"])
        saved = json.loads((folder / "session.json").read_text(encoding="utf-8"))
        self.assertEqual(saved["guide"], "jin-yuan")
        self.assertIn("攻略：金元帝国", (folder / "brief.md").read_text(encoding="utf-8"))
        code, out = run(["context", str(folder), "--goal", "接着玩"])
        self.assertEqual(code, 0)
        guide = (folder / "agent" / "guide.md").read_text(encoding="utf-8")
        self.assertIn("# 文明Ⅱ", guide)
        self.assertIn("第六篇 无法无天", guide)
        self.assertFalse((folder / "agent" / "strategy.md").exists())
        self.assertIn("strategy.md", (folder / "agent" / "playbook.md").read_text(encoding="utf-8"))

    def test_unknown_guide_is_refused_before_a_folder(self):
        code, out = run(["new", "--request", "没有的攻略", "--guide", "no-such"])
        self.assertEqual((code, out["error"]), (2, "usage"))
        self.assertEqual(list(Path(self.tmp.name).iterdir()), [])
        code, out = run(["new", "--request", "先建一局", "--id", "refuse guide"])
        folder = Path(out["dir"])
        code, out = run(["resume", str(folder), "--guide", "no-such"])
        self.assertEqual((code, out["error"]), (2, "usage"))
        self.assertIn("jin-yuan", out["reason"])
        self.assertIsNone(json.loads((folder / "session.json").read_text(encoding="utf-8")).get("guide"))

    def test_guides_are_the_registry_and_only_those_files(self):
        code, out = run(["guides"])
        self.assertEqual(code, 0)
        self.assertEqual(out["guides"][0]["name"], "jin-yuan")
        self.assertTrue((session.GUIDE_REGISTRY.parent / "jin-yuan.md").is_file())
        self.assertTrue((session.GUIDE_REGISTRY.parent / "registry.example.json").is_file())
        regdir = Path(self.tmp.name) / "guides"
        regdir.mkdir()
        (regdir / "one.md").write_text("一份全文\n", encoding="utf-8")
        reg = regdir / "registry.json"
        reg.write_text(json.dumps({"one": {"title": "一份", "file": "one.md"}}, ensure_ascii=False), encoding="utf-8")
        self.assertEqual(session.chosen_guide("one", reg), "one")
        with self.assertRaises(session.SessionError) as caught:
            session.chosen_guide("jin-yuan", reg)
        self.assertIn("one", caught.exception.response["reason"])
        (regdir / "outside.json").write_text(json.dumps({"evil": {"title": "外", "file": "../rule.md"}}), encoding="utf-8")
        with self.assertRaises(session.SessionError) as caught:
            session.load_guides(regdir / "outside.json")
        self.assertIn("同一目录", caught.exception.response["reason"])

    def test_replacing_a_rule_keeps_the_previous_text_in_the_game(self):
        code, out = run(["new", "--request", "看法则", "--id", "rule versions", "--rule", "none"])
        self.assertEqual(code, 0)
        folder = Path(out["dir"])
        self.assertFalse((folder / "agent" / "rule.md").exists())
        first = folder / "first.md"
        first.write_text("第一版法则\n", encoding="utf-8")
        code, out = run(["rule", str(folder), "--file", str(first)])
        self.assertEqual(code, 0)
        self.assertTrue(out["changed"])
        self.assertIsNone(out["archived"])
        second = folder / "second.md"
        second.write_text("第二版法则\n", encoding="utf-8")
        code, out = run(["rule", str(folder), "--file", str(second)])
        self.assertEqual(out["archived"]["file"], "v0001.md")
        self.assertEqual((folder / "agent" / "rules" / "v0001.md").read_text(encoding="utf-8"), "第一版法则\n")
        self.assertEqual((folder / "agent" / "rule.md").read_text(encoding="utf-8"), "第二版法则\n")
        code, out = run(["rule", str(folder), "--version", "1"])
        self.assertEqual(out["text"], "第一版法则\n")
        code, out = run(["context", str(folder), "--goal", "接着"])
        self.assertEqual((folder / "agent" / "rule.md").read_text(encoding="utf-8"), "第二版法则\n")
        code, out = run(["rule", str(folder), "--use", "basic"])
        self.assertIn("先看局面再下令", out["text"])
        self.assertEqual((folder / "agent" / "rules" / "v0002.md").read_text(encoding="utf-8"), "第二版法则\n")

    def test_checkpoint_archives_the_strategy_once_per_turn(self):
        folder = Path(self.tmp.name) / "archive"
        (folder / "agent").mkdir(parents=True)
        text = "# 第 12 回合\n\n造战士。\n"
        (folder / "agent" / "strategy.md").write_text(text, encoding="utf-8")
        self.assertEqual(session.archive_strategy(folder), "T0012.md")
        saved = folder / "agent" / "strategy" / "T0012.md"
        self.assertEqual(saved.read_text(encoding="utf-8"), text)
        (folder / "agent" / "strategy.md").write_text("# 第 12 回合\n\n改成造移民。\n", encoding="utf-8")
        self.assertIsNone(session.archive_strategy(folder))
        self.assertEqual(saved.read_text(encoding="utf-8"), text)
        (folder / "agent" / "strategy.md").write_text("没有回合号\n", encoding="utf-8")
        self.assertIsNone(session.archive_strategy(folder))


if __name__ == "__main__":
    unittest.main()
