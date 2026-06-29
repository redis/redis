#!/usr/bin/env python3
import contextlib
import hashlib
import importlib.util
import io
import json
import socketserver
import sys
import tempfile
import threading
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = REPO_ROOT / "tools" / "redis-bitmap-migrate.py"
MODULE_NAME = "redis_bitmap_migrate_under_test"

spec = importlib.util.spec_from_file_location(MODULE_NAME, TOOL_PATH)
migrate = importlib.util.module_from_spec(spec)
sys.modules[MODULE_NAME] = migrate
spec.loader.exec_module(migrate)


def encode_resp(value):
    if isinstance(value, bytes):
        return b"$%d\r\n%s\r\n" % (len(value), value)
    if isinstance(value, str):
        data = value.encode()
        return b"+%s\r\n" % data
    if isinstance(value, int):
        return b":%d\r\n" % value
    if value is None:
        return b"$-1\r\n"
    if isinstance(value, list):
        return b"*%d\r\n" % len(value) + b"".join(encode_resp(item) for item in value)
    raise TypeError(value)


def read_resp_command(fp):
    line = fp.readline()
    if not line:
        return None
    if not line.startswith(b"*"):
        raise AssertionError(f"expected RESP array, got {line!r}")
    count = int(line[1:-2])
    command = []
    for _ in range(count):
        bulk = fp.readline()
        if not bulk.startswith(b"$"):
            raise AssertionError(f"expected RESP bulk, got {bulk!r}")
        size = int(bulk[1:-2])
        data = fp.read(size)
        if fp.read(2) != b"\r\n":
            raise AssertionError("bulk did not end with CRLF")
        command.append(data)
    return command


class FakeRedisServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, mode, *args, source_type="reroaring", bits=None):
        self.mode = mode
        self.source_type = source_type
        self.bits = {b"foo": set(bits if bits is not None else {1, 2, 5, 100})}
        self.target = {}
        self.commands = []
        super().__init__(*args)


class FakeRedisHandler(socketserver.BaseRequestHandler):
    def handle(self):
        fp = self.request.makefile("rb")
        while True:
            command = read_resp_command(fp)
            if command is None:
                return
            self.server.commands.append(command)
            try:
                reply = self.dispatch(command)
            except Exception as exc:
                self.request.sendall(b"-ERR " + str(exc).encode() + b"\r\n")
                continue
            self.request.sendall(encode_resp(reply))

    def dispatch(self, command):
        name = command[0].upper()
        if name == b"PING":
            return "PONG"
        if name == b"SELECT":
            return "OK"
        if name == b"INFO":
            return b"# Cluster\r\ncluster_enabled:0\r\n"
        if self.server.mode == "source":
            return self.source(command, name)
        return self.target(command, name)

    def source(self, command, name):
        key = command[1] if len(command) > 1 else b""
        if name == b"SCAN":
            return [b"0", list(self.server.bits)]
        if name == b"EXISTS":
            return 1 if key in self.server.bits else 0
        if name.startswith(b"R64.") and self.server.source_type != "roaring64":
            raise RuntimeError("WRONGTYPE")
        if name.startswith(b"R.") and self.server.source_type != "reroaring":
            raise RuntimeError("WRONGTYPE")
        if name in {b"R.BITCOUNT", b"R64.BITCOUNT"}:
            return len(self.server.bits[key])
        if name in {b"R.MIN", b"R64.MIN"}:
            values = self.server.bits[key]
            return min(values) if values else -1
        if name in {b"R.MAX", b"R64.MAX"}:
            values = self.server.bits[key]
            return max(values) if values else -1
        if name == b"PEXPIRETIME":
            return -1
        if name == b"DUMP":
            return b"source-payload"
        if name in {b"R.RANGEINTARRAY", b"R64.RANGEINTARRAY"}:
            values = sorted(self.server.bits[key])
            start = int(command[2])
            end = int(command[3])
            return values[start:end + 1]
        raise RuntimeError(f"unknown source command {name.decode()}")

    def target(self, command, name):
        key = command[1] if len(command) > 1 else b""
        if name == b"EXISTS":
            return 1 if key in self.server.target else 0
        if name == b"DEL":
            deleted = 0
            for item in command[1:]:
                if item in self.server.target:
                    deleted += 1
                    del self.server.target[item]
            return deleted
        if name == b"SETBIT":
            offset = int(command[2])
            value = int(command[3])
            bits = self.server.target.setdefault(key, set())
            old = 1 if offset in bits else 0
            if value:
                bits.add(offset)
            else:
                bits.discard(offset)
            return old
        if name == b"BITMAP":
            return "OK"
        if name == b"DUMP":
            bits = sorted(self.server.target[key])
            return b"payload:" + b",".join(str(bit).encode() for bit in bits)
        if name == b"RESTORE":
            payload = command[3]
            values = payload.split(b":", 1)[1]
            self.server.target[key] = set() if not values else {
                int(bit) for bit in values.split(b",") if bit
            }
            return "OK"
        if name == b"TYPE":
            return "bitmap" if key in self.server.target else "none"
        if name == b"OBJECT":
            subcommand = command[1].upper()
            object_key = command[2] if len(command) > 2 else b""
            if subcommand == b"ENCODING" and object_key in self.server.target:
                return b"bitmap-roaring"
            return None
        if name == b"BITCOUNT":
            return len(self.server.target[key])
        if name == b"BITPOS":
            return min(self.server.target[key]) if self.server.target[key] else -1
        if name == b"GETBIT":
            return 1 if int(command[2]) in self.server.target.get(key, set()) else 0
        if name == b"PTTL":
            return -1
        if name == b"RENAME":
            self.server.target[command[2]] = self.server.target.pop(command[1])
            return "OK"
        if name == b"RENAMENX":
            source, destination = command[1], command[2]
            if destination in self.server.target:
                return 0
            self.server.target[destination] = self.server.target.pop(source)
            return 1
        raise RuntimeError(f"unknown target command {name.decode()}")


class ServerPair:
    def __init__(self, source_type="reroaring", bits=None):
        self.source = FakeRedisServer(
            "source",
            ("127.0.0.1", 0),
            FakeRedisHandler,
            source_type=source_type,
            bits=bits,
        )
        self.target = FakeRedisServer("target", ("127.0.0.1", 0), FakeRedisHandler)
        self.threads = [
            threading.Thread(target=self.source.serve_forever, daemon=True),
            threading.Thread(target=self.target.serve_forever, daemon=True),
        ]

    def __enter__(self):
        for thread in self.threads:
            thread.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.source.shutdown()
        self.target.shutdown()
        self.source.server_close()
        self.target.server_close()

    @property
    def source_port(self):
        return self.source.server_address[1]

    @property
    def target_port(self):
        return self.target.server_address[1]

    def args(self, manifest, *extra):
        return [
            "--source-host", "127.0.0.1",
            "--source-port", str(self.source_port),
            "--target-host", "127.0.0.1",
            "--target-port", str(self.target_port),
            "--manifest", str(manifest),
            *extra,
        ]


class RedisBitmapMigrateTests(unittest.TestCase):
    def run_migrator(self, args):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            rc = migrate.main(args)
        return rc, stdout.getvalue(), stderr.getvalue()

    def test_apply_migrates_reroaring_key(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={1, 2, 5, 100}) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--apply", "--assume-frozen", "--page-size", "2")
            )

            self.assertEqual(rc, 0)
            self.assertIn("MIGRATED db=0 key=foo count=4", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target[b"foo"], {1, 2, 5, 100})
            self.assertFalse(any(key.endswith(b":build") or key.endswith(b":tmp") for key in servers.target.target))

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "committed")
            self.assertEqual(data["entries"][0]["cardinality"], 4)
            self.assertEqual(data["entries"][0]["validation"]["type"], "bitmap")

    def test_dry_run_records_plan_without_target_writes(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={3, 9}) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(servers.args(manifest, "--key", "foo"))

            self.assertEqual(rc, 0)
            self.assertIn("DRY-RUN db=0 key=foo type=reroaring count=2", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target, {})

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "dry_run")
            self.assertEqual(data["entries"][0]["cardinality"], 2)

    def test_roaring64_above_cap_fails_by_default(self):
        wide = migrate.NATIVE_V1_MAX_BIT + 1
        with tempfile.TemporaryDirectory() as tmp, ServerPair(source_type="roaring64", bits={wide}) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(servers.args(manifest, "--source-type", "auto"))

            self.assertEqual(rc, 1)
            self.assertEqual(stdout, "")
            self.assertIn("FAILED db=0 key=foo", stderr)
            self.assertIn("exceeds v1 native bitmap cap", stderr)
            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "failed")
            self.assertIn(str(wide), data["entries"][0]["error"])

    def test_roaring64_above_cap_can_skip_loudly(self):
        wide = migrate.NATIVE_V1_MAX_BIT + 1
        with tempfile.TemporaryDirectory() as tmp, ServerPair(source_type="roaring64", bits={wide}) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(servers.args(manifest, "--cap-policy", "skip"))

            self.assertEqual(rc, 0)
            self.assertIn("skipped=1", stdout)
            self.assertIn("SKIP db=0 key=foo", stderr)
            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "skipped")
            self.assertEqual(data["entries"][0]["cap_policy"], "skip")
            self.assertIn(str(wide), data["entries"][0]["error"])

    def test_resume_imported_temp_key_commits_without_reexport(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={1, 2, 5, 100}) as servers:
            manifest = Path(tmp) / "manifest.json"
            key = b"foo"
            entry_id = "db0:" + hashlib.sha256(key).hexdigest()
            digest = hashlib.sha256(b"0:" + key).hexdigest().encode("ascii")
            temp_key = b"__redis-bitmap-migrate:" + digest + b":tmp"
            servers.target.target[temp_key] = {1, 2, 5, 100}

            manifest.write_text(json.dumps({
                "version": migrate.MANIFEST_VERSION,
                "created_at_ms": 1,
                "updated_at_ms": 1,
                "tool": "redis-bitmap-migrate",
                "options": {},
                "entries": [{
                    "id": entry_id,
                    "state": "imported",
                    "db": 0,
                    "key": "foo",
                    "key_b64": "Zm9v",
                    "sample_offsets": [1, 100],
                    "full_diff_offsets": [1, 2, 5, 100],
                }],
            }))

            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--resume", "--apply", "--assume-frozen")
            )

            self.assertEqual(rc, 0)
            self.assertIn("RESUMED db=0 key=foo from temporary key", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target[b"foo"], {1, 2, 5, 100})
            self.assertNotIn(temp_key, servers.target.target)
            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "committed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
