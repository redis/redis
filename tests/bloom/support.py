"""Small stdlib harness for Bloom flow cases ported from RedisBloom v8.11.81.

Keeps upstream assertions readable without redis-py, RLTest, or readies.
Each test gets disposable native Redis servers; no module checkout is needed.
"""
import functools
import os
from pathlib import Path
import shlex
import socket
import sys
import time
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration"))
from bloom import Client, RedisError as ResponseError, Server

VALGRIND = os.environ.get("VALGRIND") == "1"
environments = []


def large_memory(fn):
    @functools.wraps(fn)
    def wrapped(*args, **kwargs):
        if os.environ.get("BLOOM_LARGE_TESTS") != "1":
            raise unittest.SkipTest("set BLOOM_LARGE_TESTS=1 for large-memory/stress cases")
        return fn(*args, **kwargs)
    return wrapped


def decode(value):
    if isinstance(value, bytes):
        return value.decode()
    if isinstance(value, list):
        return [decode(v) for v in value]
    if isinstance(value, dict):
        return {decode(k): decode(v) for k, v in value.items()}
    return value


class Expectation:
    def __init__(self, env, args):
        self.env = env
        self.failed = False
        try:
            self.value = env.cmd(*args)
        except ResponseError as error:
            self.value = str(error)
            self.failed = True

    def error(self):
        self.env.assertTrue(self.failed, f"Expected error, got {self.value!r}")
        return self

    def contains(self, value):
        self.env.assertIn(value, self.value)
        return self

    def notContains(self, value):
        self.env.assertNotIn(value, self.value)
        return self

    def equal(self, value):
        self.env.assertFalse(self.failed, self.value)
        self.env.assertEqual(value, self.value)
        return self

    def ok(self):
        self.env.assertFalse(self.failed, self.value)
        self.env.assertOk(self.value)
        return self

    def true(self):
        self.env.assertFalse(self.failed, self.value)
        self.env.assertTrue(self.value)
        return self


class Env(unittest.TestCase):
    def assertAlmostEqual(self, first, second, places=7, msg=None, delta=None):
        # RLTest also permits an absolute floating-point tolerance as argument 3.
        if isinstance(places, float) and delta is None:
            delta, places = places, None
        return super().assertAlmostEqual(first, second, places=places, msg=msg, delta=delta)

    def __init__(self, decodeResponses=False, protocol=2, extra=(), useSlaves=False, freshEnv=False):
        super().__init__()
        self.replica = None
        if useSlaves:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', 0))
                port = probe.getsockname()[1]
            extra = (*extra, '--bind', '127.0.0.1', '--port', str(port))
        self.server = Server(extra=extra)
        self.decode_responses = decodeResponses
        self.protocol = protocol
        environments.append(self)
        if useSlaves:
            self.replica = Server()
            self.replica.client.command('REPLICAOF', '127.0.0.1', port)
            for _ in range(600):
                if b'master_link_status:up' in self.replica.client.command('INFO', 'replication'):
                    break
                time.sleep(.025)
            else:
                self.fail('Replication startup timed out')
        if protocol == 3:
            self.server.client.command("HELLO", 3)

    def cmd(self, command, *args):
        parts = shlex.split(command) if isinstance(command, str) else [command]
        value = self.server.client.command(*parts, *args)
        # Match the two redis-py response conversions used by upstream tests.
        if parts[0].upper() == "PING":
            return value == b"PONG"
        if parts[0].upper() == "INFO":
            result = {}
            for line in value.decode().splitlines():
                if ":" not in line or line.startswith("#"):
                    continue
                key, field = line.split(":", 1)
                try:
                    field = float(field) if "." in field else int(field)
                except ValueError:
                    pass
                result[key] = field
            return result
        return decode(value) if self.decode_responses else value

    execute_command = cmd

    def expect(self, *args):
        return Expectation(self, args)

    def assertOk(self, value):
        self.assertIn(value, (b"OK", "OK", True))

    def assertResponseError(self, value=None, contained=None):
        if value is None:
            return self.assertRaisesRegex(ResponseError, contained) if contained else self.assertRaises(ResponseError)
        self.assertIsInstance(value, ResponseError)
        if contained is not None:
            self.assertIn(contained, str(value))

    def dumpAndReload(self):
        self.cmd("SAVE")
        self.server.stop()
        self.server.start()
        if self.protocol == 3:
            self.server.client.command("HELLO", 3)

    def skip(self, reason="Skipped upstream (capacity exceeds supported limit)"):
        raise unittest.SkipTest(reason)

    restartAndReload = dumpAndReload

    def skipOnSlave(self):
        pass  # This harness always runs commands on its own primary.

    def isCluster(self):
        return False

    def getConnection(self):
        return SimpleNamespace(execute_command=self.cmd)

    def getSlaveConnection(self):
        def command(*args):
            value = self.replica.client.command(*args)
            return decode(value) if self.decode_responses else value
        return SimpleNamespace(execute_command=command)

    def skipOnVersionSmaller(self, version):
        if not server_version_at_least(self, version):
            self.skip(f"requires Redis {version}")

    def debugPrint(self, message):
        if os.environ.get("BLOOM_VERBOSE"):
            print(message)


def server_version_at_least(env, version):
    actual = str(env.cmd("INFO", "server")["redis_version"])
    return tuple(map(int, actual.split("."))) >= tuple(map(int, version.split(".")))


def server_version_less_than(env, version):
    return not server_version_at_least(env, version)


def close_environments():
    errors = []
    try:
        for env in reversed(environments):
            for server in (env.replica, env.server):
                if server:
                    try:
                        server.close()
                    except Exception as error:
                        errors.append(error)
    finally:
        environments.clear()
    if errors:
        raise errors[0]
