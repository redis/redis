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
        if kind == b",":
            return float(value)
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
    def __init__(self, binary=SERVER, extra=(), configfile=None):
        self.temp = tempfile.TemporaryDirectory(prefix="bf-")
        self.directory = Path(self.temp.name)
        self.path = self.directory / "s"
        self.binary, self.extra = binary, extra
        self.configfile = configfile
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
        # Match the core Tcl harness: allocation-failure tests need NULL,
        # not ASan/MSan's default fatal error for oversized allocations.
        server_env = os.environ.copy()
        for option in ("ASAN_OPTIONS", "MSAN_OPTIONS"):
            server_env[option] = server_env.get(option, "") + ":allocator_may_return_null=1"
        self.process = subprocess.Popen([
            str(self.binary), *([str(self.configfile)] if self.configfile else []),
            "--port", "0", "--unixsocket", str(self.path),
            "--dir", str(self.directory), "--save", "", "--enable-debug-command", "yes",
            *self.extra,
        ], stdout=self.log, stderr=subprocess.STDOUT, env=server_env)
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
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        if self.client:
            self.client.close()
        self.log.close()
        if self.process is not None and self.process.returncode != 0:
            raise AssertionError(
                f"Redis exited with status {self.process.returncode}:\n"
                + (self.directory / "log").read_text(errors="replace"))

    def close(self):
        try:
            self.stop()
        finally:
            self.temp.cleanup()
