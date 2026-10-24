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
        self.dir = root / "games" / "g1"
        (self.dir / "saves").mkdir(parents=True)
        self.old_root = session.GAME_ROOT
        session.GAME_ROOT = self.game
        os.environ["CIV2AGENT_GAMES_DIR"] = str(root / "games")

    def tearDown(self):
        session.GAME_ROOT = self.old_root
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


if __name__ == "__main__":
    unittest.main()
