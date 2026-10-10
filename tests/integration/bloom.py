#!/usr/bin/env python3
"""Integration tests using only Python's standard library.

REDIS_SERVER selects a BUILD_BLOOM=yes binary. Optional BLOOM_ORACLE_SERVER
and BLOOM_ORACLE_MODULE enable differential tests against external RedisBloom.
"""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
SERVER = Path(os.environ.get("REDIS_SERVER", ROOT / "src/redis-server")).resolve()


class RedisError(Exception):
    pass


class Client:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        try:
            self.socket.connect(str(path))
        except OSError:
            self.socket.close()
            raise
        self.stream = self.socket.makefile("rb")

    def close(self):
        self.stream.close()
        self.socket.close()

    def read(self, raise_errors=True):
        line = self.stream.readline()
        if not line:
            raise EOFError("Redis closed connection")
        kind, value = line[:1], line[1:-2]
        if kind == b"-":
            message = value.decode()
            error = RedisError(message[4:] if message.startswith("ERR ") else message)
            if raise_errors:
                raise error
            return error
        if kind == b"+":
            return value
        if kind == b":":
            return int(value)
        if kind == b"#":
            return value == b"t"
        if kind == b"_":
            return None
        if kind == b"$":
            length = int(value)
            if length == -1:
                return None
            data = self.stream.read(length)
            assert self.stream.read(2) == b"\r\n"
            return data
        if kind == b"*":
            if int(value) == -1:
                return None
            return [self.read(False) for _ in range(int(value))]
        if kind == b"%":
            return {self.read(): self.read() for _ in range(int(value))}
        raise AssertionError(f"Unsupported response: {line!r}")

    def command(self, *args):
        args = [v if isinstance(v, bytes) else str(v).encode() for v in args]
        packet = b"*%d\r\n" % len(args)
        packet += b"".join(b"$%d\r\n" % len(v) + v + b"\r\n" for v in args)
        self.socket.sendall(packet)
        return self.read()


class Server:
    def __init__(self, binary=SERVER, extra=()):
        self.temp = tempfile.TemporaryDirectory(prefix="bf-")
        self.directory = Path(self.temp.name)
        self.path = self.directory / "s"
        self.binary, self.extra = binary, extra
        self.process = None
        self.client = None
        try:
            self.start()
        except BaseException:
            if self.process is not None and self.process.poll() is None:
                self.process.terminate()
                self.process.wait(timeout=10)
            self.log.close()
            self.temp.cleanup()
            raise

    def start(self):
        self.log = open(self.directory / "log", "ab")
        self.process = subprocess.Popen([
            str(self.binary), "--port", "0", "--unixsocket", str(self.path),
            "--dir", str(self.directory), "--save", "", "--enable-debug-command", "yes",
            *self.extra,
        ], stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(200):
            if self.process.poll() is not None:
                raise AssertionError((self.directory / "log").read_text())
            try:
                self.client = Client(self.path)
                self.client.command("PING")
                return
            except (OSError, EOFError):
                time.sleep(0.025)
            except RedisError as error:
                self.client.close()
                self.client = None
                if not str(error).startswith("LOADING"):
                    raise
                time.sleep(0.025)
        raise AssertionError("Redis startup timed out")

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            try:
                self.client.command("SHUTDOWN", "NOSAVE")
            except (EOFError, OSError):
                pass
            except RedisError:
                self.process.terminate()
            self.process.wait(timeout=10)
        if self.client:
            self.client.close()
        self.log.close()

    def close(self):
        self.stop()
        self.temp.cleanup()


def snapshot(client, key):
    chunks = []
    iterator = 0
    while True:
        iterator, data = client.command("BF.SCANDUMP", key, iterator)
        if not iterator:
            return chunks
        chunks.append((iterator, data))


class BloomTests(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.addCleanup(self.server.close)
        self.r = self.server.client.command

    def test_commands_binary_and_resp3(self):
        self.assertEqual(0, self.r("BF.EXISTS", "missing", "a"))
        self.assertEqual(0, self.r("EXISTS", "missing"))
        self.assertEqual(b"OK", self.r("BF.RESERVE", "bf", 0.01, 100))
        for item in (b"", b"a\x00b\xff", b"x" * 2048):
            self.assertEqual(1, self.r("BF.ADD", "bf", item))
            self.assertEqual(0, self.r("BF.ADD", "bf", item))
            self.assertEqual(1, self.r("BF.EXISTS", "bf", item))
        self.r("HELLO", 3)
        self.assertIs(True, self.r("BF.EXISTS", "bf", b""))
        self.assertIs(True, self.r("BF.ADD", "auto", "new"))
        self.assertIs(False, self.r("BF.ADD", "auto", "new"))

    def test_scaling_and_roundtrip(self):
        self.r("BF.RESERVE", "bf", 0.001, 2)
        for i in range(500):
            self.r("BF.ADD", "bf", i)
        self.assertGreater(self.r("MEMORY", "USAGE", "bf"), 0)
        for iterator, data in snapshot(self.server.client, "bf"):
            self.r("BF.LOADCHUNK", "copy", iterator, data)
        self.assertEqual(snapshot(self.server.client, "bf"), snapshot(self.server.client, "copy"))
        payload = self.r("DUMP", "bf")
        self.r("RESTORE", "restored", 0, payload)
        self.r("SAVE")
        self.server.stop()
        self.server.start()
        self.r = self.server.client.command
        for key in ("bf", "copy", "restored"):
            for i in range(500):
                self.assertEqual(1, self.r("BF.EXISTS", key, i))

    def test_native_object_lifecycle(self):
        self.r("BF.RESERVE", "bf", 0.000001, 2)
        for i in range(100):
            self.r("BF.ADD", "bf", i)
        self.assertEqual(b"bloom", self.r("TYPE", "bf"))
        self.assertNotIn(b"module", self.r("COMMAND", "INFO", "BF.ADD")[0][2])
        self.assertFalse(any(b"bf" in entry for entry in self.r("MODULE", "LIST")))
        self.assertEqual([b"bf"], self.r("SCAN", 0, "TYPE", "bloom")[1])
        digest = self.r("DEBUG", "DIGEST-VALUE", "bf")
        self.assertEqual(1, self.r("COPY", "bf", "copy"))
        self.assertEqual(digest, self.r("DEBUG", "DIGEST-VALUE", "copy"))
        # Bloom membership is probabilistic: choose an item absent before COPY.
        item = next((f"copy-only-{i}" for i in range(1000)
                     if not self.r("BF.EXISTS", "bf", f"copy-only-{i}")), None)
        self.assertIsNotNone(item)
        self.assertEqual(1, self.r("BF.ADD", "copy", item))
        self.assertEqual(0, self.r("BF.EXISTS", "bf", item))
        self.assertEqual(digest, self.r("DEBUG", "DIGEST-VALUE", "bf"))
        self.assertNotEqual(digest, self.r("DEBUG", "DIGEST-VALUE", "copy"))
        self.assertEqual(b"OK", self.r("RENAME", "copy", "renamed"))
        self.assertEqual(1, self.r("UNLINK", "renamed"))
        self.assertEqual(1, self.r("EXPIRE", "bf", 0))
        self.assertEqual(0, self.r("DBSIZE"))
        # Option-like key names are not options.
        self.r("BF.RESERVE", "EXPANSION", 0.001, 2)
        self.assertEqual(b"bloom", self.r("TYPE", "EXPANSION"))

    def test_native_notifications(self):
        self.r("CONFIG", "SET", "notify-keyspace-events", "EA")
        listener = Client(self.server.path)
        try:
            listener.command("SUBSCRIBE", "__keyevent@0__:bf.add")
            self.r("BF.ADD", "bf", "a")
            self.assertEqual([b"message", b"__keyevent@0__:bf.add", b"bf"], listener.read())
        finally:
            listener.close()

    def test_nonscaling_and_invalid_arguments(self):
        for args in (("NONSCALING",), ("EXPANSION", 0)):
            self.r("DEL", "bf")
            self.r("BF.RESERVE", "bf", 0.001, 1, *args)
            self.assertEqual(1, self.r("BF.ADD", "bf", "a"))
            with self.assertRaisesRegex(RedisError, "non scaling filter is full"):
                self.r("BF.ADD", "bf", "b")
        for error in ("nan", "inf", "0", "1", "-1"):
            with self.assertRaises(RedisError):
                self.r("BF.RESERVE", "invalid", error, 10)
        self.assertEqual(0, self.r("EXISTS", "invalid"))
        self.r("SET", "string", "x")
        self.assertEqual(0, self.r("BF.EXISTS", "string", "x"))
        with self.assertRaisesRegex(RedisError, "WRONGTYPE"):
            self.r("BF.ADD", "string", "x")
        with self.assertRaises(RedisError):
            self.r("BF.LOADCHUNK", "invalid", 1, b"corrupt")
        self.assertEqual(b"PONG", self.r("PING"))

    def test_aof_rewrite(self):
        for preamble in ("no", "yes"):
            s = Server(extra=("--appendonly", "yes", "--aof-use-rdb-preamble", preamble))
            try:
                r = s.client.command
                for i in range(200):
                    r("BF.ADD", "bf", i)
                r("BGREWRITEAOF")
                for _ in range(400):
                    info = r("INFO", "persistence")
                    if b"aof_rewrite_in_progress:0" in info and b"aof_rewrite_scheduled:0" in info:
                        break
                    time.sleep(0.025)
                else:
                    self.fail("AOF rewrite timed out")
                self.assertIn(b"aof_last_bgrewrite_status:ok", info)
                s.stop()
                s.start()
                for i in range(200):
                    self.assertEqual(1, s.client.command("BF.EXISTS", "bf", i))
            finally:
                s.close()

    def test_config_metadata_and_watch(self):
        self.r("CONFIG", "SET", "bf-initial-size", 1, "bf-expansion-factor", 0,
               "bf-error-rate", 0.001)
        self.assertEqual(1, self.r("BF.ADD", "configured", "a"))
        with self.assertRaisesRegex(RedisError, "non scaling filter is full"):
            self.r("BF.ADD", "configured", "b")
        with self.assertRaises(RedisError):
            self.r("CONFIG", "SET", "bf-error-rate", "nan")
        self.r("CONFIG", "SET", "bf-error-rate", 0.5)
        self.assertEqual([b"bf-error-rate", b"0.25"], self.r("CONFIG", "GET", "bf-error-rate"))
        for command, arity in (("BF.RESERVE", -4), ("BF.ADD", 3), ("BF.EXISTS", 3),
                               ("BF.SCANDUMP", 3), ("BF.LOADCHUNK", 4)):
            self.assertEqual(arity, self.r("COMMAND", "INFO", command)[0][1])
        self.assertEqual([b"key"], self.r("COMMAND", "GETKEYS", "BF.ADD", "key", "item"))
        other = Client(self.server.path)
        try:
            self.r("WATCH", "watched")
            other.command("BF.ADD", "watched", "item")
            self.r("MULTI")
            self.r("PING")
            self.assertIsNone(self.r("EXEC"))
        finally:
            other.close()

    def test_corrupt_chunk_headers(self):
        self.r("BF.ADD", "bf", "item")
        _, valid = self.r("BF.SCANDUMP", "bf", 0)
        for offset, fmt, value in ((8, "I", 0), (12, "I", 0xFFFFFFFF), (16, "I", 0),
                                   (20, "Q", 0), (44, "d", float("nan"))):
            corrupt = bytearray(valid)
            struct.pack_into("=" + fmt, corrupt, offset, value)
            with self.assertRaises(RedisError):
                self.r("BF.LOADCHUNK", "invalid", 1, bytes(corrupt))
            self.assertEqual(0, self.r("EXISTS", "invalid"))
        for length in (0, 1, 19, len(valid) - 1):
            with self.assertRaises(RedisError):
                self.r("BF.LOADCHUNK", "invalid", 1, valid[:length])
        self.assertEqual(b"PONG", self.r("PING"))

    def test_replication_and_acl(self):
        # Redis replication uses TCP; bind only loopback for this test.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        primary = Server(extra=("--bind", "127.0.0.1", "--port", str(port)))
        replica = Server()
        try:
            primary.client.command("BF.ADD", "bf", "before-sync")
            replica.client.command("REPLICAOF", "127.0.0.1", port)
            for _ in range(600):
                if b"master_link_status:up" in replica.client.command("INFO", "replication"):
                    break
                time.sleep(0.025)
            else:
                self.fail("Replica synchronization timed out")
            primary.client.command("BF.ADD", "bf", "after-sync")
            primary.client.command("BF.MADD", "bf", "multi-one", "multi-two")
            primary.client.command("BF.INSERT", "inserted", "CAPACITY", 2, "ITEMS", "x", "y", "z")
            self.assertEqual(1, primary.client.command("WAIT", 1, 5000))
            for item in ("before-sync", "after-sync", "multi-one", "multi-two"):
                self.assertEqual(1, replica.client.command("BF.EXISTS", "bf", item))
            self.assertEqual([1, 1, 1], replica.client.command("BF.MEXISTS", "inserted", "x", "y", "z"))
        finally:
            replica.close()
            primary.close()
        self.r("ACL", "SETUSER", "reader", "on", ">password", "~*", "+bf.exists")
        self.r("AUTH", "reader", "password")
        self.assertEqual(0, self.r("BF.EXISTS", "missing", "x"))
        with self.assertRaisesRegex(RedisError, "NOPERM"):
            self.r("BF.ADD", "bf", "x")
        self.r("AUTH", "default", "")

    @unittest.skipUnless(os.environ.get("BLOOM_ORACLE_SERVER") and os.environ.get("BLOOM_ORACLE_MODULE"),
                         "Set BLOOM_ORACLE_SERVER and BLOOM_ORACLE_MODULE for compatibility tests")
    def test_external_module_compatibility(self):
        oracle = Server(binary=os.environ["BLOOM_ORACLE_SERVER"],
                        extra=("--loadmodule", os.environ["BLOOM_ORACLE_MODULE"]))
        try:
            for client in (self.server.client, oracle.client):
                client.command("BF.RESERVE", "bf", 0.001, 2)
                for i in range(500):
                    client.command("BF.ADD", "bf", i)
            self.assertEqual(snapshot(self.server.client, "bf"), snapshot(oracle.client, "bf"))
            for source, destination in ((self.server.client, oracle.client),
                                        (oracle.client, self.server.client)):
                payload = source.command("DUMP", "bf")
                destination.command("RESTORE", "imported", 0, payload)
                for i in range(500):
                    self.assertEqual(1, destination.command("BF.EXISTS", "imported", i))
        finally:
            oracle.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
