"""Regression tests: python -m unittest discover -s utils/req-res-validator."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "validator", ROOT / "utils/req-res-log-validator.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class ArgumentTests(unittest.TestCase):
    def request(self, command, values):
        words = command.split("|")
        with (ROOT / "src/commands" / ("-".join(words) + ".json")).open() as f:
            doc = json.load(f)[words[-1].upper()]
        # Exercise the flags representation returned by COMMAND DOCS.
        def convert_flags(args):
            for arg in args:
                arg["flags"] = [flag for flag in ("optional", "multiple", "multiple_token")
                                if arg.pop(flag, False)]
                convert_flags(arg.get("arguments", []))
        convert_flags(doc.get("arguments", []))
        return SimpleNamespace(command=command, argv=words + values, doc=doc)

    def test_repeated_options(self):
        for options in (["GET", "EX", "10", "GET"],
                        ["EX", "10", "EX", "20"],
                        ["NX", "GET", "NX"]):
            with self.subTest(options=options):
                self.assertIsNone(validator.validate_request(
                    self.request("set", ["k", "v"] + options)))

    def test_long_argument_list(self):
        self.assertIsNone(validator.validate_request(
            self.request("eval", ["return 1", "0"] + ["x"] * 1100)))

    def test_repeated_option_values_are_checked(self):
        for options in (["EX", "10", "EX", "bad"], ["EX", "10", "EX"],
                        ["GET", "unknown"]):
            with self.subTest(options=options):
                self.assertIsNotNone(validator.validate_request(
                    self.request("set", ["k", "v"] + options)))

    def test_digest_type_mutation(self):
        for command, values in (("set", ["k", "v"]), ("delex", ["k"])):
            for token in ("IFDEQ", "IFDNE"):
                with self.subTest(command=command, token=token):
                    req = self.request(command, values + [token, "abcdef0123456789"])
                    self.assertIsNone(validator.validate_request(req))
                    req.doc = copy.deepcopy(req.doc)
                    for arg in req.doc["arguments"]:
                        for choice in arg.get("arguments", []):
                            if choice.get("token") == token:
                                choice["type"] = "integer"
                    self.assertIsNotNone(validator.validate_request(req))

    def test_multiple_tokens_and_blocks(self):
        for command, values in (
                ("sort", ["missing", "GET", "#", "ALPHA", "GET", "#"]),
                ("bitfield", ["bits", "SET", "i8", "0", "1", "GET", "i8", "0",
                              "SET", "i8", "8", "2", "GET", "i8", "8"]),
                ("bitfield_ro", ["bits", "GET", "i8", "0", "GET", "i8", "8"]),
                ("cluster|migration", ["IMPORT", "0", "1", "3", "4"]),
                ("mset", ["k", "v"] * 1100),
                ("zinter", ["2", "a", "b", "WEIGHTS", "1", "2"])):
            with self.subTest(command=command):
                self.assertIsNone(validator.validate_request(self.request(command, values)))

    def test_pure_tokens_without_explicit_token(self):
        for values in (["HELP"], ["crash-after-election", "crash-after-promotion"]):
            with self.subTest(values=values):
                self.assertIsNone(validator.validate_request(
                    self.request("sentinel|simulate-failure", values)))

    def test_repeated_blocks_require_child_tokens(self):
        for command in ("bitfield", "bitfield_ro"):
            self.assertIsNotNone(validator.validate_request(
                self.request(command, ["bits", "GET", "i8", "0", "i8", "8"])))

    def test_required_options_can_be_interleaved(self):
        for command, values in (
                ("xreadgroup", ["CLAIM", "50", "GROUP", "g", "c", "COUNT", "1",
                                "STREAMS", "s", ">"]),
                ("argrep", ["a", "-", "+", "OR", "MATCH", "redis", "LIMIT", "3",
                            "GLOB", "*array*", "AND", "LIMIT", "1", "NOCASE"])):
            with self.subTest(command=command):
                self.assertIsNone(validator.validate_request(self.request(command, values)))
        self.assertIsNotNone(validator.validate_request(
            self.request("argrep", ["a", "-", "+", "OR", "LIMIT", "3"])))

    def test_numeric_wire_formats(self):
        for command, values in (
                ("bitfield", ["bits", "SET", "u8", "#0", "65"]),
                ("zcount", ["z", "(0", "(+inf"]),
                ("zadd", ["z", "0x1p+0", "one"]),
                ("zincrby", ["z", "1.5", "member"])):
            with self.subTest(command=command):
                self.assertIsNone(validator.validate_request(self.request(command, values)))

    def test_numeric_prefixes_do_not_hide_type_mismatches(self):
        for value, wrong_type in (("#1", "integer"), ("(1.5", "double")):
            with self.subTest(value=value):
                req = self.request("set", ["k", value])
                self.assertIsNone(validator.validate_request(req))
                req.doc["arguments"][1]["type"] = wrong_type
                self.assertIsNotNone(validator.validate_request(req))
        for command, values in (
                ("bitfield", ["bits", "SET", "u8", "#0", "#1"]),
                ("zrangebyscore", ["z", "(0", "(1", "LIMIT", "#0", "1"])):
            with self.subTest(command=command):
                self.assertIsNotNone(validator.validate_request(self.request(command, values)))

    def test_nested_options_preserve_parent_requirement(self):
        for values in (
                ["TO", "127.0.0.1", "6379", "FORCE", "TIMEOUT", "100"],
                ["TO", "127.0.0.1", "6379", "TIMEOUT", "100", "FORCE"],
                ["FORCE", "TIMEOUT", "100", "TO", "127.0.0.1", "6379"]):
            with self.subTest(values=values):
                self.assertIsNone(validator.validate_request(self.request("failover", values)))
        self.assertIsNotNone(validator.validate_request(
            self.request("failover", ["FORCE", "TIMEOUT", "100"])))

    def test_unknown_metrics_do_not_skip_other_argument_checks(self):
        prefix = ["METRICS", "2", "GPU", "CPU"]
        self.assertIsNone(validator.validate_request(
            self.request("hotkeys|start", prefix + ["COUNT", "10", "DURATION", "20"])))
        for option in ("COUNT", "DURATION", "SAMPLE"):
            with self.subTest(option=option):
                self.assertIsNotNone(validator.validate_request(
                    self.request("hotkeys|start", prefix + [option, "bad"])))
        self.assertIsNotNone(validator.validate_request(
            self.request("hotkeys|start", ["METRICS", "bad", "CPU"])))
        self.assertIsNotNone(validator.validate_request(
            self.request("hotkeys|start", prefix + ["GPU"])))

    def test_internal_commands_without_argument_docs(self):
        self.assertIsNone(validator.validate_request(
            self.request("replconf", ["listening-port", "12345"])))

    def test_command_documentation_regressions(self):
        for command, values in (
                ("client|list", ["TYPE", "slave"]),
                ("module|loadex", ["module.so", "CONFIG", "option", "value", "ARGS"]),
                ("sentinel|set", ["mymaster", "rename-command", "PING", "PONG"])):
            with self.subTest(command=command):
                self.assertIsNone(validator.validate_request(self.request(command, values)))

    def test_every_reply_is_validated(self):
        request = "4\r\nPING\r\n12\r\n__argv_end__\r\n"
        docs = {"ping": {"reply_schema": {"type": "integer"}}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.reqres"
            path.write_bytes((request + ":1\r\n" + request + "+OK\r\n").encode())
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(validator.jsonschema.ValidationError):
                    validator.process_file(docs, path)

    def test_invalid_reply_schema_is_rejected(self):
        docs = {"ping": {"reply_schema": {"type": "invalid"}}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.reqres"
            path.write_bytes(b"4\r\nPING\r\n12\r\n__argv_end__\r\n:1\r\n")
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(validator.jsonschema.SchemaError):
                    validator.process_file(docs, path)


if __name__ == "__main__":
    unittest.main()
