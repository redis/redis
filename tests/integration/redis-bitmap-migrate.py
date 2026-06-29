#!/usr/bin/env python3
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import warnings
from pathlib import Path
from urllib.request import urlretrieve
from zipfile import ZipFile


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = REPO_ROOT / "tools" / "redis-bitmap-migrate.py"
MODULE_NAME = "redis_bitmap_migrate_under_test"
REALDATA_BASE_URL = "https://raw.githubusercontent.com/RoaringBitmap/real-roaring-datasets/master"
INTEGER_RE = re.compile(r"\d+")

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

    def __init__(
        self,
        mode,
        *args,
        source_type="reroaring",
        bits=None,
        expire_at_ms=None,
        mutate_after_range=False,
    ):
        self.mode = mode
        self.source_type = source_type
        if bits is None:
            self.bits = {b"foo": {1, 2, 5, 100}}
        elif isinstance(bits, dict):
            self.bits = {
                key if isinstance(key, bytes) else str(key).encode("utf-8"): set(values)
                for key, values in bits.items()
            }
        else:
            self.bits = {b"foo": set(bits)}
        if expire_at_ms is None:
            self.source_expire_at_ms = {}
        elif isinstance(expire_at_ms, dict):
            self.source_expire_at_ms = {
                key if isinstance(key, bytes) else str(key).encode("utf-8"): int(value)
                for key, value in expire_at_ms.items()
            }
        else:
            self.source_expire_at_ms = {key: int(expire_at_ms) for key in self.bits}
        self.mutate_after_range = mutate_after_range
        self.mutated_range_keys = set()
        self.target = {}
        self.target_byte_len = {}
        self.target_expire_at_ms = {}
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
            return self.server.source_expire_at_ms.get(key, -1)
        if name == b"DUMP":
            return b"source-payload"
        if name in {b"R.RANGEINTARRAY", b"R64.RANGEINTARRAY"}:
            values = sorted(self.server.bits[key])
            start = int(command[2])
            end = int(command[3])
            reply = values[start:end + 1]
            if self.server.mutate_after_range and key not in self.server.mutated_range_keys:
                next_offset = max(values) + 1 if values else 0
                self.server.bits[key].add(next_offset)
                self.server.mutated_range_keys.add(key)
            return reply
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
                    self.server.target_byte_len.pop(item, None)
                    self.server.target_expire_at_ms.pop(item, None)
            return deleted
        if name == b"SET":
            value = command[2]
            self.server.target[key] = {
                bit
                for byte_index, byte in enumerate(value)
                for bit_index in range(8)
                if byte & (1 << (7 - bit_index))
                for bit in [byte_index * 8 + bit_index]
            }
            self.server.target_byte_len[key] = len(value)
            self.server.target_expire_at_ms.pop(key, None)
            return "OK"
        if name == b"SETBIT":
            offset = int(command[2])
            value = int(command[3])
            bits = self.server.target.setdefault(key, set())
            old = 1 if offset in bits else 0
            self.server.target_byte_len[key] = max(
                self.target_byte_len(key),
                offset // 8 + 1,
            )
            if value:
                bits.add(offset)
            else:
                bits.discard(offset)
            return old
        if name == b"BITMAP":
            return "OK"
        if name == b"DUMP":
            bits = sorted(self.server.target[key])
            return (
                b"payload:"
                + str(self.target_byte_len(key)).encode()
                + b":"
                + b",".join(str(bit).encode() for bit in bits)
            )
        if name == b"RESTORE":
            ttl = int(command[2])
            payload = command[3]
            raw_byte_len, values = payload.split(b":", 2)[1:]
            self.server.target[key] = set() if not values else {
                int(bit) for bit in values.split(b",") if bit
            }
            self.server.target_byte_len[key] = int(raw_byte_len)
            options = {part.upper() for part in command[4:]}
            if ttl > 0 and b"ABSTTL" in options:
                self.server.target_expire_at_ms[key] = ttl
            elif ttl > 0:
                self.server.target_expire_at_ms[key] = migrate.now_ms() + ttl
            else:
                self.server.target_expire_at_ms.pop(key, None)
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
            bit = int(command[2])
            bits = self.server.target[key]
            if bit == 1:
                return min(bits) if bits else -1
            for offset in range(self.target_byte_len(key) * 8):
                if offset not in bits:
                    return offset
            return -1
        if name == b"GETBIT":
            return 1 if int(command[2]) in self.server.target.get(key, set()) else 0
        if name == b"PTTL":
            if key not in self.server.target:
                return -2
            expire_at = self.server.target_expire_at_ms.get(key)
            if expire_at is None:
                return -1
            return max(expire_at - migrate.now_ms(), 0)
        if name == b"RENAME":
            source, destination = command[1], command[2]
            source_byte_len = self.target_byte_len(source)
            source_expire_at = self.server.target_expire_at_ms.pop(source, None)
            self.server.target[destination] = self.server.target.pop(source)
            self.server.target_byte_len.pop(destination, None)
            self.server.target_expire_at_ms.pop(destination, None)
            self.server.target_byte_len[destination] = self.server.target_byte_len.pop(
                source,
                source_byte_len,
            )
            if source_expire_at is not None:
                self.server.target_expire_at_ms[destination] = source_expire_at
            return "OK"
        if name == b"RENAMENX":
            source, destination = command[1], command[2]
            if destination in self.server.target:
                return 0
            source_byte_len = self.target_byte_len(source)
            source_expire_at = self.server.target_expire_at_ms.pop(source, None)
            self.server.target[destination] = self.server.target.pop(source)
            self.server.target_byte_len[destination] = self.server.target_byte_len.pop(
                source,
                source_byte_len,
            )
            if source_expire_at is not None:
                self.server.target_expire_at_ms[destination] = source_expire_at
            return 1
        raise RuntimeError(f"unknown target command {name.decode()}")

    def target_byte_len(self, key):
        if key in self.server.target_byte_len:
            return self.server.target_byte_len[key]
        bits = self.server.target.get(key, set())
        return max(bits) // 8 + 1 if bits else 0


class ServerPair:
    def __init__(
        self,
        source_type="reroaring",
        bits=None,
        expire_at_ms=None,
        mutate_after_range=False,
    ):
        self.source = FakeRedisServer(
            "source",
            ("127.0.0.1", 0),
            FakeRedisHandler,
            source_type=source_type,
            bits=bits,
            expire_at_ms=expire_at_ms,
            mutate_after_range=mutate_after_range,
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
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", ResourceWarning)
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

    def test_apply_preserves_empty_source_as_empty_native_bitmap(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits=set()) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--apply", "--assume-frozen", "--page-size", "2")
            )

            self.assertEqual(rc, 0)
            self.assertIn("MIGRATED db=0 key=foo count=0", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target[b"foo"], set())
            self.assertEqual(servers.target.target_byte_len[b"foo"], 0)

            target = migrate.RedisClient(migrate.RedisEndpoint(
                "127.0.0.1",
                servers.target_port,
                0,
                None,
                None,
                1.0,
                "target",
            ))
            self.assertEqual(target.execute(["BITPOS", "foo", "0"]), -1)
            self.assertEqual(target.execute(["BITPOS", "foo", "1"]), -1)

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "committed")
            self.assertEqual(data["entries"][0]["cardinality"], 0)
            self.assertEqual(data["entries"][0]["validation"]["bitpos_0"], -1)
            self.assertEqual(data["entries"][0]["validation"]["bitpos_1"], -1)

    def test_apply_restores_absolute_ttl(self):
        expire_at = migrate.now_ms() + 60000
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={7, 9}, expire_at_ms=expire_at) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--apply", "--assume-frozen")
            )

            self.assertEqual(rc, 0)
            self.assertIn("MIGRATED db=0 key=foo count=2", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target[b"foo"], {7, 9})
            self.assertEqual(servers.target.target_expire_at_ms[b"foo"], expire_at)

            restore_commands = [
                command for command in servers.target.commands
                if command and command[0].upper() == b"RESTORE"
            ]
            self.assertEqual(len(restore_commands), 1)
            self.assertEqual(restore_commands[0][2], str(expire_at).encode())
            self.assertIn(b"ABSTTL", [part.upper() for part in restore_commands[0]])

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["expire_at_ms"], expire_at)
            self.assertGreater(data["entries"][0]["validation"]["ttl_ms"], 0)

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

    def test_apply_uses_requested_db_for_source_and_target(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={3, 9}) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--db", "2", "--key", "foo", "--apply", "--assume-frozen")
            )

            self.assertEqual(rc, 0)
            self.assertIn("MIGRATED db=2 key=foo count=2", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target[b"foo"], {3, 9})
            self.assertIn([b"SELECT", b"2"], servers.source.commands)
            self.assertIn([b"SELECT", b"2"], servers.target.commands)

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["db"], 2)

    def test_key_file_accepts_binary_key_names(self):
        key = b"bitmap:\xff native"
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={key: {4, 12}}) as servers:
            manifest = Path(tmp) / "manifest.json"
            key_file = Path(tmp) / "keys.txt"
            key_file.write_bytes(key + b"\n")
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--key-file", str(key_file), "--apply", "--assume-frozen")
            )

            self.assertEqual(rc, 0)
            self.assertIn("count=2", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target[key], {4, 12})

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["key_b64"], migrate.key_to_b64(key))
            self.assertEqual(data["entries"][0]["destination_key_b64"], migrate.key_to_b64(key))

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

    def test_apply_without_replace_preserves_existing_destination_and_fails(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={1, 2, 5}) as servers:
            manifest = Path(tmp) / "manifest.json"
            servers.target.target[b"foo"] = {77}
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--apply", "--assume-frozen")
            )

            self.assertEqual(rc, 1)
            self.assertEqual(stdout, "")
            self.assertIn("FAILED db=0 key=foo", stderr)
            self.assertIn("destination key already exists", stderr)
            self.assertEqual(servers.target.target[b"foo"], {77})
            self.assertFalse(any(key.endswith(b":build") or key.endswith(b":tmp") for key in servers.target.target))

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "failed")
            self.assertEqual(data["entries"][0]["overwrite_policy"], "no-overwrite")
            self.assertIn("destination key already exists", data["entries"][0]["error"])

    def test_apply_replace_overwrites_existing_destination(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={1, 2, 5}) as servers:
            manifest = Path(tmp) / "manifest.json"
            servers.target.target[b"foo"] = {77}
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--apply", "--assume-frozen", "--replace")
            )

            self.assertEqual(rc, 0)
            self.assertIn("MIGRATED db=0 key=foo count=3", stdout)
            self.assertEqual(stderr, "")
            self.assertEqual(servers.target.target[b"foo"], {1, 2, 5})

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "committed")
            self.assertEqual(data["entries"][0]["overwrite_policy"], "replace")

    def test_source_cardinality_change_during_export_fails(self):
        with tempfile.TemporaryDirectory() as tmp, ServerPair(bits={1, 2, 5, 100}, mutate_after_range=True) as servers:
            manifest = Path(tmp) / "manifest.json"
            rc, stdout, stderr = self.run_migrator(
                servers.args(manifest, "--apply", "--assume-frozen", "--page-size", "2")
            )

            self.assertEqual(rc, 1)
            self.assertEqual(stdout, "")
            self.assertIn("source cardinality changed during export", stderr)
            self.assertNotIn(b"foo", servers.target.target)

            data = json.loads(manifest.read_text())
            self.assertEqual(data["entries"][0]["state"], "failed")
            self.assertIn("source cardinality changed during export", data["entries"][0]["error"])

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


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def redis_server_path():
    return Path(os.environ.get("REDIS_SERVER", REPO_ROOT / "src" / "redis-server"))


def redis_roaring_module_path():
    raw = os.environ.get("REDIS_ROARING_MODULE")
    if raw:
        return Path(raw)
    return REPO_ROOT.parent / "redis-roaring" / "build" / "libredis-roaring.so"


def env_truthy(name):
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}


def croaring_realdata_dir():
    return Path(os.environ.get("REDIS_CROARING_REALDATA_DIR", REPO_ROOT.parent / "croaring-realdata"))


def parse_realdata_text(text, max_values):
    bits = []
    for match in INTEGER_RE.finditer(text):
        bits.append(int(match.group(0)))
        if len(bits) >= max_values:
            break
    return bits


def load_census1881_realdata_bits(max_values=4096, min_values=128):
    realdata_path = croaring_realdata_dir()
    candidates = []
    if realdata_path.is_file():
        candidates.append(realdata_path)
    elif realdata_path.is_dir():
        archive = realdata_path / "census1881.zip"
        if archive.is_file():
            candidates.append(archive)
        candidates.extend(
            path for path in sorted(realdata_path.rglob("*"))
            if path.is_file() and "census1881" in str(path).lower()
        )

    if not candidates and env_truthy("REDIS_DOWNLOAD_CROARING_REALDATA"):
        realdata_path.mkdir(parents=True, exist_ok=True)
        archive = realdata_path / "census1881.zip"
        if not archive.exists():
            urlretrieve(f"{REALDATA_BASE_URL}/census1881.zip", archive)
        candidates.append(archive)

    merged_bits = set()
    sources = []
    for path in candidates:
        if path.suffix.lower() == ".zip":
            with ZipFile(path) as zf:
                for member in sorted(name for name in zf.namelist() if not name.endswith("/")):
                    with zf.open(member) as fp:
                        text = fp.read().decode("utf-8", "replace")
                    parsed = parse_realdata_text(text, max_values)
                    if parsed:
                        merged_bits.update(parsed)
                        sources.append(f"{path.name}:{member}")
                    if len(merged_bits) >= max_values:
                        break
        elif path.suffix.lower() in {"", ".txt", ".csv"}:
            parsed = parse_realdata_text(path.read_text(encoding="utf-8", errors="replace"), max_values)
            if parsed:
                merged_bits.update(parsed)
                sources.append(str(path))
        if len(merged_bits) >= max_values:
            break

    if len(merged_bits) >= min_values:
        return sorted(merged_bits)[:max_values], ",".join(sources[:4])

    raise unittest.SkipTest(
        "CRoaring census1881 realdata not found; set REDIS_CROARING_REALDATA_DIR "
        "or REDIS_DOWNLOAD_CROARING_REALDATA=1"
    )


def real_redis_client(port, name):
    return migrate.RedisClient(migrate.RedisEndpoint(
        "127.0.0.1",
        port,
        0,
        None,
        None,
        2.0,
        name,
    ))


class RedisProcess:
    def __init__(self, server_path, port, data_dir, module_path=None):
        self.server_path = Path(server_path)
        self.port = port
        self.data_dir = Path(data_dir)
        self.module_path = Path(module_path) if module_path is not None else None
        self.proc = None

    def __enter__(self):
        self.data_dir.mkdir(parents=True, exist_ok=True)
        args = [
            str(self.server_path),
            "--port", str(self.port),
            "--bind", "127.0.0.1",
            "--protected-mode", "no",
            "--save", "",
            "--appendonly", "no",
            "--dir", str(self.data_dir),
            "--dbfilename", "dump.rdb",
            "--loglevel", "warning",
        ]
        if self.module_path is not None:
            args.extend(["--loadmodule", str(self.module_path)])
        self.proc = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.wait_ready()
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.proc is None:
            return
        try:
            if self.proc.poll() is None:
                try:
                    real_redis_client(self.port, "shutdown").execute(["SHUTDOWN", "NOSAVE"])
                except Exception:
                    pass
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.terminate()
                    try:
                        self.proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.proc.kill()
                        self.proc.wait(timeout=5)
        finally:
            if self.proc.stdout is not None:
                self.proc.stdout.close()

    def wait_ready(self):
        client = real_redis_client(self.port, "redis-server")
        deadline = time.time() + 10
        last_error = None
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"redis-server exited early:\n{self.output()}")
            try:
                if client.execute(["PING"]) == "PONG":
                    return
            except Exception as exc:
                last_error = exc
                time.sleep(0.05)
        raise RuntimeError(f"redis-server did not start: {last_error}")

    def output(self):
        if self.proc is None or self.proc.stdout is None:
            return ""
        if self.proc.poll() is None:
            return ""
        return self.proc.stdout.read()


class RealRedisRoaringMigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server_path = redis_server_path()
        cls.module_path = redis_roaring_module_path()
        missing = []
        if not cls.server_path.is_file():
            missing.append(f"REDIS_SERVER/src redis-server not found: {cls.server_path}")
        if not cls.module_path.is_file():
            missing.append(f"REDIS_ROARING_MODULE not found: {cls.module_path}")
        if missing:
            raise unittest.SkipTest("; ".join(missing))

    def run_migrator(self, args):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", ResourceWarning)
                rc = migrate.main(args)
        return rc, stdout.getvalue(), stderr.getvalue()

    def start_pair(self, tmp):
        source_port = free_tcp_port()
        target_port = free_tcp_port()
        source = RedisProcess(
            self.server_path,
            source_port,
            Path(tmp) / "source",
            self.module_path,
        )
        target = RedisProcess(self.server_path, target_port, Path(tmp) / "target")
        return source, target, source_port, target_port

    def migrator_args(self, source_port, target_port, manifest, key, *extra):
        return [
            "--source-host", "127.0.0.1",
            "--source-port", str(source_port),
            "--target-host", "127.0.0.1",
            "--target-port", str(target_port),
            "--manifest", str(manifest),
            "--key", key,
            "--apply",
            "--assume-frozen",
            "--page-size", "2",
            "--socket-timeout", "2",
            *extra,
        ]

    def migrator_scan_args(self, source_port, target_port, manifest, *extra):
        return [
            "--source-host", "127.0.0.1",
            "--source-port", str(source_port),
            "--target-host", "127.0.0.1",
            "--target-port", str(target_port),
            "--manifest", str(manifest),
            "--apply",
            "--assume-frozen",
            "--socket-timeout", "2",
            *extra,
        ]

    def assert_native_bits(self, client, key, set_bits, clear_bits, expected_count=None):
        self.assertEqual(migrate.decode_text(client.execute(["TYPE", key])), "bitmap")
        self.assertIn(
            "bitmap",
            migrate.decode_text(client.execute(["OBJECT", "ENCODING", key])),
        )
        if expected_count is None:
            expected_count = len(set_bits)
        self.assertEqual(client.execute(["BITCOUNT", key]), expected_count)
        for bit in set_bits:
            self.assertEqual(client.execute(["GETBIT", key, str(bit)]), 1)
        for bit in clear_bits:
            self.assertEqual(client.execute(["GETBIT", key, str(bit)]), 0)

    def assert_native_dataset(self, client, key, bits):
        self.assertGreater(len(bits), 0)
        probe_bits = bits[:4] + bits[len(bits) // 2:len(bits) // 2 + 4] + bits[-4:]
        self.assert_native_bits(
            client,
            key,
            probe_bits,
            self.clear_bit_samples(bits),
            expected_count=len(bits),
        )
        self.assertEqual(client.execute(["BITPOS", key, "1"]), bits[0])
        self.assertEqual(client.execute(["GETBIT", key, str(bits[-1])]), 1)

    def seed_roaring_int_array(self, client, command_prefix, key, bits, chunk_size=512):
        client.execute(["DEL", key])
        for index in range(0, len(bits), chunk_size):
            chunk = bits[index:index + chunk_size]
            command = f"{command_prefix}.SETINTARRAY" if index == 0 else f"{command_prefix}.APPENDINTARRAY"
            client.execute([command, key, *chunk])

    def seed_reroaring_int_array(self, client, key, bits, chunk_size=512):
        self.seed_roaring_int_array(client, "R", key, bits, chunk_size)

    def seed_roaring64_int_array(self, client, key, bits, chunk_size=512):
        self.seed_roaring_int_array(client, "R64", key, bits, chunk_size)

    def clear_bit_samples(self, bits, limit=8):
        present = set(bits)
        samples = []
        candidate = 0
        while len(samples) < limit:
            if candidate not in present:
                samples.append(candidate)
            candidate += 1
        return samples

    def test_real_reroaring_migration_preserves_bits_and_ttl(self):
        bits = {1, 2, 5, 100, 65536}
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "manifest.json"
            source_proc, target_proc, source_port, target_port = self.start_pair(tmp)
            with source_proc, target_proc:
                source = real_redis_client(source_port, "source")
                target = real_redis_client(target_port, "target")
                for bit in sorted(bits):
                    source.execute(["R.SETBIT", "foo", str(bit), "1"])
                source.execute(["PEXPIRE", "foo", "60000"])
                source_ttl = source.execute(["PTTL", "foo"])

                rc, stdout, stderr = self.run_migrator(
                    self.migrator_args(source_port, target_port, manifest, "foo")
                )

                self.assertEqual(rc, 0)
                self.assertIn("MIGRATED db=0 key=foo count=5", stdout)
                self.assertEqual(stderr, "")
                self.assert_native_bits(target, "foo", bits, {0, 3, 99, 65535})
                target_ttl = target.execute(["PTTL", "foo"])
                self.assertGreater(target_ttl, 0)
                self.assertLessEqual(target_ttl, source_ttl + 1000)

            data = json.loads(manifest.read_text())
            entry = data["entries"][0]
            self.assertEqual(entry["state"], "committed")
            self.assertEqual(entry["source_type"], "reroaring")
            self.assertEqual(entry["cardinality"], len(bits))
            self.assertGreater(entry["validation"]["ttl_ms"], 0)

    def test_real_roaring64_migration_preserves_in_cap_bits(self):
        bits = {0, 63, 4096, 131072}
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "manifest.json"
            source_proc, target_proc, source_port, target_port = self.start_pair(tmp)
            with source_proc, target_proc:
                source = real_redis_client(source_port, "source")
                target = real_redis_client(target_port, "target")
                for bit in sorted(bits):
                    source.execute(["R64.SETBIT", "wide", str(bit), "1"])

                rc, stdout, stderr = self.run_migrator(
                    self.migrator_args(source_port, target_port, manifest, "wide")
                )

                self.assertEqual(rc, 0)
                self.assertIn("MIGRATED db=0 key=wide count=4", stdout)
                self.assertEqual(stderr, "")
                self.assert_native_bits(target, "wide", bits, {1, 64, 4097})

            data = json.loads(manifest.read_text())
            entry = data["entries"][0]
            self.assertEqual(entry["state"], "committed")
            self.assertEqual(entry["source_type"], "roaring64")
            self.assertEqual(entry["cardinality"], len(bits))

    def test_real_croaring_census1881_reroaring_migration_preserves_dataset(self):
        bits, source_name = load_census1881_realdata_bits()
        self.assertGreater(len(bits), 100)
        self.assertLessEqual(max(bits), 0xFFFFFFFF)

        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "manifest.json"
            source_proc, target_proc, source_port, target_port = self.start_pair(tmp)
            with source_proc, target_proc:
                source = real_redis_client(source_port, "source")
                target = real_redis_client(target_port, "target")
                self.seed_reroaring_int_array(source, "census1881", bits)
                self.assertEqual(source.execute(["R.BITCOUNT", "census1881"]), len(bits))

                rc, stdout, stderr = self.run_migrator(
                    self.migrator_args(
                        source_port,
                        target_port,
                        manifest,
                        "census1881",
                        "--page-size", "256",
                        "--pipeline-size", "256",
                    )
                )

                self.assertEqual(rc, 0)
                self.assertIn(f"MIGRATED db=0 key=census1881 count={len(bits)}", stdout)
                self.assertEqual(stderr, "")
                self.assert_native_dataset(target, "census1881", bits)

            data = json.loads(manifest.read_text())
            entry = data["entries"][0]
            self.assertEqual(entry["state"], "committed")
            self.assertEqual(entry["source_type"], "reroaring")
            self.assertEqual(entry["cardinality"], len(bits))
            self.assertEqual(entry["validation"]["cardinality"], len(bits))
            self.assertIn("census1881", source_name)

    def test_real_croaring_census1881_scan_migrates_multiple_prefixed_keys(self):
        bits, source_name = load_census1881_realdata_bits(max_values=768, min_values=256)
        datasets = {
            "census1881:even": bits[::2],
            "census1881:odd": bits[1::2],
        }

        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "manifest.json"
            source_proc, target_proc, source_port, target_port = self.start_pair(tmp)
            with source_proc, target_proc:
                source = real_redis_client(source_port, "source")
                target = real_redis_client(target_port, "target")
                for key, key_bits in datasets.items():
                    self.seed_reroaring_int_array(source, key, key_bits, chunk_size=128)
                    self.assertEqual(source.execute(["R.BITCOUNT", key]), len(key_bits))
                self.seed_reroaring_int_array(source, "outside-census1881", bits[:128], chunk_size=128)

                rc, stdout, stderr = self.run_migrator(
                    self.migrator_scan_args(
                        source_port,
                        target_port,
                        manifest,
                        "--pattern", "census1881:*",
                        "--target-prefix", "native:",
                        "--page-size", "64",
                        "--pipeline-size", "64",
                    )
                )

                self.assertEqual(rc, 0)
                self.assertEqual(stderr, "")
                for key, key_bits in datasets.items():
                    self.assertIn(f"MIGRATED db=0 key={key} count={len(key_bits)}", stdout)
                    self.assert_native_dataset(target, "native:" + key, key_bits)
                self.assertEqual(target.execute(["EXISTS", "native:outside-census1881"]), 0)

            data = json.loads(manifest.read_text())
            entries = sorted(data["entries"], key=lambda entry: entry["key"])
            self.assertEqual([entry["key"] for entry in entries], sorted(datasets))
            for entry in entries:
                key_bits = datasets[entry["key"]]
                self.assertEqual(entry["state"], "committed")
                self.assertEqual(entry["source_type"], "reroaring")
                self.assertEqual(entry["destination_key"], "native:" + entry["key"])
                self.assertEqual(entry["cardinality"], len(key_bits))
                self.assertEqual(entry["validation"]["cardinality"], len(key_bits))
                self.assertEqual(len(entry["full_diff_offsets"]), len(key_bits))
            self.assertIn("census1881", source_name)

    def test_real_croaring_census1881_roaring64_migration_preserves_dataset(self):
        bits, source_name = load_census1881_realdata_bits(max_values=512, min_values=128)
        self.assertGreater(len(bits), 100)
        self.assertLessEqual(max(bits), 0xFFFFFFFF)

        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "manifest.json"
            source_proc, target_proc, source_port, target_port = self.start_pair(tmp)
            with source_proc, target_proc:
                source = real_redis_client(source_port, "source")
                target = real_redis_client(target_port, "target")
                self.seed_roaring64_int_array(source, "census1881-64", bits, chunk_size=128)
                self.assertEqual(source.execute(["R64.BITCOUNT", "census1881-64"]), len(bits))

                rc, stdout, stderr = self.run_migrator(
                    self.migrator_args(
                        source_port,
                        target_port,
                        manifest,
                        "census1881-64",
                        "--page-size", "128",
                        "--pipeline-size", "128",
                    )
                )

                self.assertEqual(rc, 0)
                self.assertIn(f"MIGRATED db=0 key=census1881-64 count={len(bits)}", stdout)
                self.assertEqual(stderr, "")
                self.assert_native_dataset(target, "census1881-64", bits)

            data = json.loads(manifest.read_text())
            entry = data["entries"][0]
            self.assertEqual(entry["state"], "committed")
            self.assertEqual(entry["source_type"], "roaring64")
            self.assertEqual(entry["cardinality"], len(bits))
            self.assertEqual(entry["validation"]["cardinality"], len(bits))
            self.assertIn("census1881", source_name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
