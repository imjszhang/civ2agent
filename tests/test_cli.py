#!/usr/bin/env python3
"""Offline tests for cli/civ2ctl.py: run without the game.

python -m unittest discover -s civ2agent/tests -p "test_*.py"
"""

import contextlib
import importlib.util
import io
import json
import unittest
from pathlib import Path

CLI = Path(__file__).resolve().parents[1] / "cli" / "civ2ctl.py"
spec = importlib.util.spec_from_file_location("civ2ctl", CLI)
civ2ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(civ2ctl)


def build(argv):
    """The pipe request a command line turns into, without sending it."""
    args = civ2ctl.build_parser().parse_args(argv)
    command = civ2ctl.BY_NAME[args.cmd]
    return command.build(command, args)


def run_cli(argv, pipe=r"\\.\pipe\civ2agent-test-absent"):
    """Exit code and parsed stdout of one call, against a pipe nobody serves."""
    old = civ2ctl.PIPE
    civ2ctl.PIPE = pipe
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            code = civ2ctl.main(argv)
    finally:
        civ2ctl.PIPE = old
    lines = out.getvalue().splitlines()
    return code, [json.loads(line) for line in lines]


class RequestMapping(unittest.TestCase):
    def test_flags_become_pipe_fields(self):
        self.assertEqual(build(["move", "--unit", "3", "--x", "4", "--y", "2"]),
                         {"op": "act", "name": "move", "unit": 3, "x": 4, "y": 2})
        self.assertEqual(build(["order", "--unit", "1", "--order", "irrigate"]),
                         {"op": "act", "name": "order", "unit": 1, "order": "irrigate"})
        self.assertEqual(build(["respond", "--text", "确定", "--edit", "罗马"]),
                         {"op": "act", "name": "respond", "text": "确定", "edit": "罗马"})
        self.assertEqual(build(["found-city", "--unit", "0"]), {"op": "act", "name": "found_city", "unit": 0})
        self.assertEqual(build(["state"]), {"op": "snapshot"})
        self.assertEqual(build(["dialog"]), {"op": "menu"})
        self.assertEqual(build(["status"]), {"op": "hello"})

    def test_defaults_and_fixed_fields(self):
        self.assertEqual(build(["end-turn"]), {"op": "end_turn", "timeout_ms": 120000})
        self.assertEqual(build(["skip-intro"]), {"op": "skip_intro", "timeout_ms": 60000})
        self.assertEqual(build(["produce", "--city", "2", "--item", "战士"]),
                         {"op": "act", "name": "produce", "timeout_ms": 20000, "city": 2, "item": "战士"})
        self.assertEqual(build(["rates", "--tax", "4", "--science", "6"]),
                         {"op": "act", "name": "rates", "timeout_ms": 20000, "tax": 4, "science": 6})

    def test_cli_only_params_stay_out_of_the_request(self):
        self.assertEqual(build(["state", "--out", "s.json"]), {"op": "snapshot"})


class Manifest(unittest.TestCase):
    def test_every_tool_is_a_command_with_the_same_parameters(self):
        manifest = civ2ctl.manifest()
        self.assertTrue(manifest["ok"])
        self.assertEqual(manifest["protocol"], civ2ctl.PROTOCOL)
        parser = civ2ctl.build_parser()
        subparsers = next(a for a in parser._actions if a.dest == "cmd").choices
        names = [tool["name"] for tool in manifest["tools"]]
        self.assertEqual(names, [c.name for c in civ2ctl.COMMANDS])
        for tool in manifest["tools"]:
            schema = tool["input_schema"]
            self.assertFalse(schema["additionalProperties"])
            flags = {a.dest for a in subparsers[tool["name"]]._actions}
            for prop in schema["properties"]:
                self.assertIn(prop, flags, f"{tool['name']}.{prop}")
            for prop in schema["required"]:
                self.assertIn(prop, schema["properties"])

    def test_cli_only_params_are_not_in_the_schema(self):
        state = next(t for t in civ2ctl.manifest()["tools"] if t["name"] == "state")
        self.assertNotIn("out", state["input_schema"]["properties"])


class CallArguments(unittest.TestCase):
    def test_call_builds_the_same_request_as_flags(self):
        command, args = civ2ctl.call_args("move", '{"unit": 3, "x": 4, "y": 2}')
        self.assertEqual(command.build(command, args), build(["move", "--unit", "3", "--x", "4", "--y", "2"]))
        command, args = civ2ctl.call_args("end-turn", "{}")
        self.assertEqual(command.build(command, args), {"op": "end_turn", "timeout_ms": 120000})

    def test_call_rejects_bad_arguments(self):
        for name, text in (("move", '{"unit": 1, "x": 2}'),          # missing y
                           ("move", '{"unit": true, "x": 2, "y": 3}'),  # bool is not an integer
                           ("move", '{"unit": 1, "x": 2, "y": 3, "z": 0}'),
                           ("order", '{"unit": 1, "order": "teleport"}'),
                           ("move", "[1, 2]"),
                           ("move", "{nope"),
                           ("nope", "{}")):
            with self.subTest(name=name, text=text):
                with self.assertRaises(civ2ctl.UsageError):
                    civ2ctl.call_args(name, text)


class ExitCodes(unittest.TestCase):
    def test_usage_errors_exit_2_with_json(self):
        for argv in (["respond"], ["respond", "--option", "1", "--text", "x"], ["move", "--unit", "1"],
                     ["produce", "--city", "1", "--item", "a", "--item-id", "2"], ["nope"],
                     ["call", "nope"]):
            with self.subTest(argv=argv):
                code, out = run_cli(argv)
                self.assertEqual(code, 2)
                self.assertEqual(len(out), 1)
                self.assertEqual(out[0]["error"], "usage")

    def test_old_names_point_to_the_new_ones(self):
        for argv, word in ((["hello"], "status"), (["snapshot", "-o", "x"], "state"), (["menu"], "dialog"),
                           (["act", "respond", "--button", "0"], "--option"), (["act", "move"], "move"),
                           (["act", "research", "--tech", "3"], "respond"), (["--timeout", "1", "hello"], "status")):
            with self.subTest(argv=argv):
                code, out = run_cli(argv)
                self.assertEqual(code, 2)
                self.assertIn(word, out[0]["reason"])

    def test_unreachable_game_exits_3_with_json(self):
        for argv in (["status"], ["--timeout", "0.3", "state"], ["move", "--unit", "1", "--x", "2", "--y", "2",
                                                                  "--timeout", "0.3"],
                     ["wait-ready", "--wait-s", "1"]):
            with self.subTest(argv=argv):
                code, out = run_cli(argv)
                self.assertEqual(code, 3)
                self.assertEqual(out[0]["error"], "not_connected")
                self.assertFalse(out[0]["ok"])

    def test_tools_exits_0(self):
        code, out = run_cli(["tools"])
        self.assertEqual(code, 0)
        self.assertTrue(out[0]["ok"])


if __name__ == "__main__":
    unittest.main()
