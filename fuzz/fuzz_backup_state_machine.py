#!/usr/bin/env python3
"""Randomized, process-level fault injection for the Redis BACKUP lifecycle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path
from typing import Any, Iterable


SANITIZER_MARKERS = (
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "runtime error:",
    "ERROR: LeakSanitizer",
    "REDIS BUG REPORT",
)
VALID_BACKUP_STATES = {
    "idle",
    "pending",
    "snapshotting",
    "incrementing",
    "sealed",
    "failed",
}


class FuzzFailure(RuntimeError):
    pass


class RedisReplyError(RuntimeError):
    pass


class RespConnection:
    def __init__(self, path: Path, timeout: float = 5.0) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(str(path))
        self.buffer = bytearray()

    def close(self) -> None:
        self.sock.close()

    def command(self, *parts: object) -> Any:
        encoded = [self._encode(part) for part in parts]
        request = bytearray(f"*{len(encoded)}\r\n", "ascii")
        for part in encoded:
            request.extend(f"${len(part)}\r\n".encode("ascii"))
            request.extend(part)
            request.extend(b"\r\n")
        self.sock.sendall(request)
        reply = self._read_reply()
        if isinstance(reply, RedisReplyError):
            raise reply
        return reply

    @staticmethod
    def _encode(value: object) -> bytes:
        if isinstance(value, bytes):
            return value
        if isinstance(value, Path):
            return os.fsencode(value)
        return str(value).encode("utf-8")

    def _read_exact(self, length: int) -> bytes:
        while len(self.buffer) < length:
            chunk = self.sock.recv(max(4096, length - len(self.buffer)))
            if not chunk:
                raise FuzzFailure("Redis closed the connection mid-reply")
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:length])
        del self.buffer[:length]
        return result

    def _read_line(self) -> bytes:
        while True:
            offset = self.buffer.find(b"\r\n")
            if offset >= 0:
                result = bytes(self.buffer[:offset])
                del self.buffer[: offset + 2]
                return result
            chunk = self.sock.recv(4096)
            if not chunk:
                raise FuzzFailure("Redis closed the connection mid-line")
            self.buffer.extend(chunk)

    def _read_reply(self) -> Any:
        prefix = self._read_exact(1)
        if prefix == b"+":
            return self._read_line().decode("utf-8", "replace")
        if prefix == b"-":
            # Keep errors as values while recursively consuming an aggregate
            # (notably EXEC). command() raises only a top-level error after the
            # complete reply has been read, so the connection stays in sync.
            return RedisReplyError(self._read_line().decode("utf-8", "replace"))
        if prefix == b":":
            return int(self._read_line())
        if prefix == b"$":
            length = int(self._read_line())
            if length == -1:
                return None
            if length < -1:
                raise FuzzFailure(f"invalid bulk reply length {length}")
            payload = self._read_exact(length)
            if self._read_exact(2) != b"\r\n":
                raise FuzzFailure("malformed bulk reply terminator")
            return payload
        if prefix in (b"*", b"~", b">"):
            length = int(self._read_line())
            if length == -1:
                return None
            if length < -1:
                raise FuzzFailure(f"invalid aggregate reply length {length}")
            return [self._read_reply() for _ in range(length)]
        if prefix == b"%":
            length = int(self._read_line())
            if length < 0:
                raise FuzzFailure(f"invalid map reply length {length}")
            return {
                self._read_reply(): self._read_reply()
                for _ in range(length)
            }
        if prefix == b"_":
            if self._read_line():
                raise FuzzFailure("malformed RESP3 null reply")
            return None
        if prefix == b"#":
            value = self._read_line()
            if value == b"t":
                return True
            if value == b"f":
                return False
        raise FuzzFailure(f"unsupported RESP reply prefix {prefix!r}")


class RedisProcess:
    def __init__(
        self,
        executable: Path,
        root: Path,
        appendonly: bool,
        rdb_preamble: bool,
        name: str = "redis",
        preload_file: Path | None = None,
        preload_type: str = "aof",
    ) -> None:
        if preload_file is not None and preload_type not in {"aof", "rdb"}:
            raise ValueError(f"unsupported preload type {preload_type!r}")
        self.executable = executable
        self.root = root
        self.name = name
        self.initial_appendonly = appendonly
        self.socket_path = root / f"{name}.sock"
        self.log_path = root / f"{name}.log"
        self.aof_dir = root / f"{name}-appendonlydir"
        self.backup_dir = root / f"{name}-backupdir"
        self.log_file = self.log_path.open("wb")
        args = [
            str(executable),
            "--port",
            "0",
            "--unixsocket",
            str(self.socket_path),
            "--unixsocketperm",
            "700",
            "--daemonize",
            "no",
            "--protected-mode",
            "no",
            "--enable-protected-configs",
            "yes",
            "--dir",
            str(root),
            "--dbfilename",
            f"{name}.rdb",
            "--save",
            "",
            "--appendonly",
            "yes" if appendonly else "no",
            "--appenddirname",
            f"{name}-appendonlydir",
            "--backupdirname",
            f"{name}-backupdir",
            "--aof-use-rdb-preamble",
            "yes" if rdb_preamble else "no",
            "--auto-aof-rewrite-percentage",
            "0",
            "--backup-sealed-ttl",
            "0",
            "--logfile",
            "",
            "--loglevel",
            "notice",
        ]
        if preload_file is not None:
            args.extend(("--preload-file", f"{preload_type}:{preload_file}"))

        env = os.environ.copy()
        env.setdefault(
            "ASAN_OPTIONS",
            "abort_on_error=1:detect_leaks=0:allocator_may_return_null=1",
        )
        env.setdefault(
            "UBSAN_OPTIONS",
            "halt_on_error=1:print_stacktrace=1",
        )
        try:
            self.process = subprocess.Popen(
                args,
                cwd=root,
                env=env,
                stdout=self.log_file,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except BaseException:
            self.log_file.close()
            raise
        # start_new_session makes the child PID the new process-group ID.
        self.process_group = self.process.pid
        self.connection: RespConnection | None = None
        try:
            self._wait_until_ready()
        except BaseException:
            self.stop(expect_clean_exit=False)
            raise

    def _wait_until_ready(self) -> None:
        deadline = time.monotonic() + 8.0
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log_file.flush()
                raise FuzzFailure(
                    f"Redis exited during startup with {self.process.returncode}:\n"
                    f"{self.read_log()}"
                )
            if self.socket_path.exists():
                try:
                    self.connection = RespConnection(self.socket_path)
                    if self.connection.command("PING") == "PONG":
                        return
                except (OSError, RuntimeError) as exc:
                    last_error = exc
                    if self.connection is not None:
                        self.connection.close()
                        self.connection = None
            time.sleep(0.02)
        raise FuzzFailure(f"Redis did not become ready: {last_error}")

    @property
    def conn(self) -> RespConnection:
        if self.connection is None:
            raise FuzzFailure("Redis connection is not available")
        return self.connection

    def assert_alive(self) -> None:
        result = self.process.poll()
        if result is not None:
            raise FuzzFailure(
                f"Redis exited unexpectedly with {result}:\n{self.read_log()}"
            )

    def children(self) -> list[int]:
        children_file = Path(
            f"/proc/{self.process.pid}/task/{self.process.pid}/children"
        )
        try:
            return [int(value) for value in children_file.read_text().split()]
        except FileNotFoundError:
            return []

    def read_log(self) -> str:
        if not self.log_file.closed:
            self.log_file.flush()
        try:
            return self.log_path.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def check_log(self) -> None:
        log = self.read_log()
        markers = [marker for marker in SANITIZER_MARKERS if marker in log]
        if markers:
            raise FuzzFailure(
                f"server log contains failure marker(s) {markers}:\n{log}"
            )

    def stop(self, expect_clean_exit: bool = True) -> None:
        forced_termination = False
        leftover_process_group = False
        if self.connection is not None:
            try:
                self.connection.command("SHUTDOWN", "NOSAVE")
            except (OSError, RuntimeError):
                pass
            self.connection.close()
            self.connection = None
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            forced_termination = True
            try:
                os.killpg(self.process_group, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.process_group, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.process.wait(timeout=3)
        finally:
            # A rewrite child belongs to the server's dedicated process group.
            # Ensure a child cannot survive a failed scenario or timeout.
            try:
                os.killpg(self.process_group, 0)
            except ProcessLookupError:
                pass
            else:
                leftover_process_group = True
                try:
                    os.killpg(self.process_group, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                try:
                    os.killpg(self.process_group, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.02)
            else:
                try:
                    os.killpg(self.process_group, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            log = self.read_log()
            self.log_file.flush()
            self.log_file.close()
            markers = [marker for marker in SANITIZER_MARKERS if marker in log]
            failures: list[str] = []
            if markers:
                failures.append(f"failure marker(s) {markers}")
            if expect_clean_exit and (
                forced_termination
                or leftover_process_group
                or self.process.returncode != 0
            ):
                failures.append(
                    "Redis did not shut down cleanly "
                    f"(returncode={self.process.returncode}, "
                    f"forced={forced_termination}, "
                    f"leftover_process_group={leftover_process_group})"
                )
            if failures:
                raise FuzzFailure(
                    f"server shutdown failed: {', '.join(failures)}:\n{log}"
                )


def as_text(value: object) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def expect_bytes(conn: RespConnection, key: str, expected: bytes) -> None:
    actual = conn.command("GET", key)
    if actual != expected:
        raise FuzzFailure(
            f"GET {key!r} returned {actual!r}, expected {expected!r}"
        )


def expect_ok(conn: RespConnection, *command: object) -> Any:
    reply = conn.command(*command)
    if reply != "OK":
        raise FuzzFailure(f"{command!r} returned {reply!r}, expected OK")
    return reply


def expect_error(
    conn: RespConnection,
    *command: object,
    contains: str | None = None,
) -> str:
    try:
        reply = conn.command(*command)
    except RedisReplyError as exc:
        message = str(exc)
        if contains is not None and contains.lower() not in message.lower():
            raise FuzzFailure(
                f"{command!r} returned error {message!r}, expected {contains!r}"
            ) from exc
        return message
    raise FuzzFailure(f"{command!r} unexpectedly returned {reply!r}")


def backup_status(conn: RespConnection) -> dict[str, str]:
    reply = conn.command("BACKUP", "STATUS")
    if isinstance(reply, dict):
        result = {as_text(key): as_text(value) for key, value in reply.items()}
    elif isinstance(reply, list) and len(reply) % 2 == 0:
        result = {
            as_text(reply[index]): as_text(reply[index + 1])
            for index in range(0, len(reply), 2)
        }
    else:
        raise FuzzFailure(f"unexpected BACKUP STATUS reply {reply!r}")
    state = result.get("state")
    if state not in VALID_BACKUP_STATES:
        raise FuzzFailure(f"invalid backup state {state!r}: {result!r}")
    return result


def info_field(conn: RespConnection, section: str, field: str) -> str:
    raw = conn.command("INFO", section)
    if not isinstance(raw, bytes):
        raise FuzzFailure(f"unexpected INFO reply {raw!r}")
    prefix = f"{field}:"
    for line in raw.decode("utf-8", "replace").splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :]
    raise FuzzFailure(f"INFO {section} omitted {field}")


def wait_for(
    server: RedisProcess,
    predicate: Any,
    description: str,
    timeout: float = 6.0,
) -> Any:
    deadline = time.monotonic() + timeout
    last_value: Any = None
    while time.monotonic() < deadline:
        server.assert_alive()
        last_value = predicate()
        if last_value:
            return last_value
        time.sleep(0.02)
    raise FuzzFailure(f"timed out waiting for {description}; last={last_value!r}")


def wait_for_state(
    server: RedisProcess,
    expected: Iterable[str],
    timeout: float = 6.0,
) -> dict[str, str]:
    expected_set = set(expected)
    result = wait_for(
        server,
        lambda: (
            status
            if (status := backup_status(server.conn))["state"] in expected_set
            else None
        ),
        f"backup state in {sorted(expected_set)}",
        timeout,
    )
    if not isinstance(result, dict):
        raise FuzzFailure(f"invalid status result {result!r}")
    return result


def mutate_dataset(conn: RespConnection, rng: random.Random, rounds: int) -> None:
    for index in range(rounds):
        selector = rng.randrange(6)
        # Operations deliberately share a small key pool so later commands can
        # encounter values created with a different type.
        key = f"fuzz:{rng.randrange(8)}"
        value = rng.randbytes(rng.randrange(0, 257))
        try:
            if selector == 0:
                conn.command("SET", key, value)
            elif selector == 1:
                conn.command("LPUSH", key, value)
            elif selector == 2:
                conn.command("HSET", key, f"field:{index % 5}", value)
            elif selector == 3:
                conn.command("SADD", key, value)
            elif selector == 4:
                conn.command("INCRBY", key, rng.randrange(-1000, 1001))
            else:
                conn.command("DEL", key)
        except RedisReplyError:
            # Reusing a key across randomly chosen types intentionally exercises
            # wrong-type paths; those command errors do not invalidate a run.
            pass


def seed_rewrite_dataset(
    conn: RespConnection,
    rng: random.Random,
    prefix: str,
    count: int = 12,
) -> None:
    for index in range(count):
        expect_ok(
            conn,
            "SET",
            f"{prefix}:{index}",
            rng.randbytes(128),
        )


def parse_aof_manifest(path: Path) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    try:
        lines = path.read_text().splitlines()
    except OSError as exc:
        raise FuzzFailure(f"cannot read AOF manifest {path}: {exc}") from exc

    for line_number, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        try:
            tokens = shlex.split(stripped)
        except ValueError as exc:
            raise FuzzFailure(
                f"cannot parse manifest {path}:{line_number}: {exc}"
            ) from exc
        if len(tokens) < 6 or len(tokens) % 2:
            raise FuzzFailure(
                f"invalid manifest entry at {path}:{line_number}: {line!r}"
            )
        entry: dict[str, str] = {}
        for index in range(0, len(tokens), 2):
            key, value = tokens[index : index + 2]
            if key in entry:
                raise FuzzFailure(
                    f"duplicate {key!r} at {path}:{line_number}"
                )
            entry[key] = value
        if not {"file", "seq", "type"} <= entry.keys():
            raise FuzzFailure(
                f"incomplete manifest entry at {path}:{line_number}: {entry!r}"
            )
        filename = entry["file"]
        if Path(filename).name != filename:
            raise FuzzFailure(
                f"manifest entry is not a basename at {path}:{line_number}: "
                f"{filename!r}"
            )
        try:
            sequence = int(entry["seq"])
        except ValueError as exc:
            raise FuzzFailure(
                f"invalid manifest sequence at {path}:{line_number}: {entry!r}"
            ) from exc
        if sequence <= 0 or entry["type"] not in {"b", "h", "i"}:
            raise FuzzFailure(
                f"invalid manifest metadata at {path}:{line_number}: {entry!r}"
            )
        entries.append(entry)

    if not entries:
        raise FuzzFailure(f"AOF manifest {path} has no entries")
    return entries


def is_base_artifact(path: Path) -> bool:
    return path.name.startswith("appendonly.aof.") and path.name.endswith(
        (".base.rdb", ".base.aof")
    )


def is_incr_artifact(path: Path) -> bool:
    return path.name.startswith("appendonly.aof.") and path.name.endswith(
        ".incr.aof"
    )


def file_fingerprint(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        return path.stat().st_size, digest.hexdigest()
    except OSError as exc:
        raise FuzzFailure(f"cannot fingerprint backup artifact {path}: {exc}") from exc


def directory_is_empty(path: Path) -> bool:
    try:
        return path.is_dir() and next(path.iterdir(), None) is None
    except OSError:
        return False


def validate_backup_directory_empty(
    server: RedisProcess,
    phase: str,
) -> None:
    wait_for(
        server,
        lambda: directory_is_empty(server.backup_dir),
        f"empty backup directory after {phase}: {server.backup_dir}",
        timeout=2.0,
    )


def backup_files(conn: RespConnection) -> list[Path]:
    reply = conn.command("BACKUP", "LIST")
    if not isinstance(reply, list):
        raise FuzzFailure(f"unexpected BACKUP LIST reply {reply!r}")
    return [Path(os.fsdecode(item)) for item in reply]


def validate_incrementing(server: RedisProcess) -> Path:
    status = wait_for_state(server, {"incrementing"})
    if status["error"]:
        raise FuzzFailure(f"incrementing backup has an error: {status!r}")
    files = backup_files(server.conn)
    if len(files) != 1 or not files[0].is_absolute():
        raise FuzzFailure(f"invalid incrementing artifact list: {files!r}")
    path = files[0].resolve()
    backup_root = server.backup_dir.resolve()
    if (
        not path.is_file()
        or path.parent != backup_root
        or not is_base_artifact(path)
    ):
        raise FuzzFailure(f"invalid incrementing BASE artifact: {files[0]}")
    return path


def validate_sealed(server: RedisProcess) -> list[Path]:
    status = backup_status(server.conn)
    if status["state"] != "sealed" or status["error"]:
        raise FuzzFailure(f"invalid sealed status: {status!r}")
    if int(status["start_time"]) <= 0 or int(status["end_time"]) < int(
        status["start_time"]
    ):
        raise FuzzFailure(f"invalid backup timestamps: {status!r}")

    files = backup_files(server.conn)
    if len(files) != 3 or len(set(files)) != 3:
        raise FuzzFailure(f"sealed backup does not expose three artifacts: {files!r}")
    backup_root = server.backup_dir.resolve()
    resolved_files: list[Path] = []
    for path in files:
        resolved = path.resolve()
        if not resolved.is_file() or resolved.parent != backup_root:
            raise FuzzFailure(f"invalid sealed backup artifact {path}")
        resolved_files.append(resolved)
    if not is_base_artifact(resolved_files[0]):
        raise FuzzFailure(f"unexpected BASE artifact {resolved_files[0]}")
    if not is_incr_artifact(resolved_files[1]):
        raise FuzzFailure(f"unexpected INCR artifact {resolved_files[1]}")
    if resolved_files[2].name != "appendonly.aof.manifest":
        raise FuzzFailure(f"unexpected manifest name {resolved_files[2]}")

    entries = parse_aof_manifest(resolved_files[2])
    expected_entries = [
        (resolved_files[0].name, "b"),
        (resolved_files[1].name, "i"),
    ]
    actual_entries = [(entry["file"], entry["type"]) for entry in entries]
    if actual_entries != expected_entries:
        raise FuzzFailure(
            f"sealed manifest entries {actual_entries!r}, "
            f"expected {expected_entries!r}"
        )
    return resolved_files


def clean_and_validate_idle(
    server: RedisProcess,
    validate_live_aof: bool = True,
) -> None:
    expect_ok(server.conn, "BACKUP", "CLEANUP")
    status = backup_status(server.conn)
    if status != {
        "state": "idle",
        "error": "",
        "start_time": "0",
        "end_time": "0",
    }:
        raise FuzzFailure(f"cleanup did not reset backup metadata: {status!r}")
    if backup_files(server.conn):
        raise FuzzFailure("BACKUP LIST is not empty after cleanup")
    validate_backup_directory_empty(server, "CLEANUP")

    config = server.conn.command("CONFIG", "GET", "appendonly")
    if not isinstance(config, list) or len(config) != 2:
        raise FuzzFailure(f"unexpected appendonly config reply: {config!r}")
    appendonly = as_text(config[1]) == "yes"
    expected_aof_state = "1" if appendonly else "0"
    actual_aof_state = info_field(server.conn, "persistence", "aof_enabled")
    if actual_aof_state != expected_aof_state:
        raise FuzzFailure(
            f"CONFIG appendonly={appendonly!r}, "
            f"but INFO aof_enabled={actual_aof_state!r}"
        )
    if appendonly and validate_live_aof:
        active_live_incr(server)
    elif not server.initial_appendonly:
        wait_for(
            server,
            lambda: directory_is_empty(server.aof_dir),
            f"empty temporary AOF directory {server.aof_dir}",
            timeout=2.0,
        )


def start_backup(server: RedisProcess) -> None:
    expect_ok(server.conn, "BACKUP", "START")


def active_live_incr(server: RedisProcess) -> Path:
    manifest = server.aof_dir / "appendonly.aof.manifest"
    entries = parse_aof_manifest(manifest)
    incr_entries = [entry for entry in entries if entry["type"] == "i"]
    if not incr_entries:
        raise FuzzFailure(f"live manifest {manifest} has no INCR entry")
    path = server.aof_dir / incr_entries[-1]["file"]
    if not path.is_file() or not is_incr_artifact(path):
        raise FuzzFailure(f"invalid active live INCR artifact {path}")
    return path


def seal_and_validate(server: RedisProcess, rng: random.Random) -> list[Path]:
    mutate_dataset(server.conn, rng, 1 + rng.randrange(8))
    expect_ok(server.conn, "BACKUP", "SEAL")
    files = validate_sealed(server)
    fingerprints = {path: file_fingerprint(path) for path in files}

    config = server.conn.command("CONFIG", "GET", "appendonly")
    if not isinstance(config, list) or len(config) != 2:
        raise FuzzFailure(f"unexpected appendonly config reply: {config!r}")
    appendonly = as_text(config[1]) == "yes"
    live_incr: Path | None = None
    live_size = 0
    if appendonly:
        live_incr = active_live_incr(server)
        if live_incr.name == files[1].name:
            raise FuzzFailure(
                f"sealed INCR remained the active live INCR: {live_incr}"
            )
        live_size = live_incr.stat().st_size

    expect_ok(server.conn, "SET", "after-seal", rng.randbytes(32))
    if appendonly:
        wait_reply = server.conn.command("WAITAOF", 1, 0, 5000)
        if (
            not isinstance(wait_reply, list)
            or len(wait_reply) != 2
            or wait_reply[0] != 1
        ):
            raise FuzzFailure(f"WAITAOF did not confirm the live AOF: {wait_reply!r}")
        if live_incr is None:
            raise FuzzFailure("appendonly is enabled without an active INCR")
        wait_for(
            server,
            lambda: live_incr.stat().st_size > live_size,
            f"post-seal write in active INCR {live_incr}",
            timeout=2.0,
        )
    else:
        if info_field(server.conn, "persistence", "aof_enabled") != "0":
            raise FuzzFailure("temporary AOF remained enabled after BACKUP SEAL")
        wait_for(
            server,
            lambda: directory_is_empty(server.aof_dir),
            f"empty temporary AOF directory {server.aof_dir}",
            timeout=2.0,
        )

    changed: dict[Path, tuple[tuple[int, str], tuple[int, str]]] = {}
    for path in files:
        current = file_fingerprint(path)
        if current != fingerprints[path]:
            changed[path] = (fingerprints[path], current)
    if changed:
        raise FuzzFailure(f"sealed backup artifacts changed after SEAL: {changed!r}")
    return files


def verify_preload(
    executable: Path,
    original: RedisProcess,
    preload_file: Path,
    expected_values: dict[str, bytes],
    absent_keys: Iterable[str],
    appendonly: bool,
    rdb_preamble: bool,
    label: str,
    preload_type: str = "aof",
) -> None:
    restore_root = Path(
        tempfile.mkdtemp(prefix="redis-backup-restore-", dir=original.root.parent)
    )
    restore: RedisProcess | None = None
    failed = False
    try:
        restore = RedisProcess(
            executable,
            restore_root,
            appendonly=appendonly,
            rdb_preamble=rdb_preamble,
            name="restore",
            preload_file=preload_file,
            preload_type=preload_type,
        )
        for key, expected in expected_values.items():
            expect_bytes(restore.conn, key, expected)
        for key in absent_keys:
            exists = restore.conn.command("EXISTS", key)
            if exists != 0:
                raise FuzzFailure(
                    f"preloaded backup unexpectedly contains {key!r}"
                )
        restore.check_log()
    except BaseException:
        failed = True
        raise
    finally:
        try:
            if restore is not None:
                restore.stop()
        except BaseException:
            failed = True
            raise
        finally:
            if failed:
                destination = original.root / f"restore-failure-{label}"
                if destination.exists():
                    shutil.rmtree(destination, ignore_errors=True)
                try:
                    restore_root.rename(destination)
                except OSError as exc:
                    active_error = sys.exc_info()[1]
                    if active_error is not None and hasattr(active_error, "add_note"):
                        active_error.add_note(
                            f"could not preserve restore root {restore_root}: {exc}"
                        )
            else:
                shutil.rmtree(restore_root, ignore_errors=True)


def mode_full(
    server: RedisProcess,
    executable: Path,
    rng: random.Random,
    rdb_preamble: bool,
) -> None:
    digest = hashlib.sha256(rng.randbytes(64)).hexdigest()
    before = digest.encode()
    during = rng.randbytes(64)
    expect_ok(server.conn, "SET", "backup-digest", before)
    start_backup(server)
    validate_incrementing(server)
    expect_ok(server.conn, "SET", "backup-during", during)
    files = seal_and_validate(server, rng)
    for restore_appendonly in (False, True):
        verify_preload(
            executable,
            server,
            files[2],
            {
                "backup-digest": before,
                "backup-during": during,
            },
            ("after-seal",),
            restore_appendonly,
            rdb_preamble,
            f"full-aof-{'yes' if restore_appendonly else 'no'}",
        )
    clean_and_validate_idle(server)


def mode_abort(server: RedisProcess, rng: random.Random) -> None:
    start_backup(server)
    wait_for_state(server, {"snapshotting", "incrementing"})
    mutate_dataset(server.conn, rng, rng.randrange(1, 8))
    expect_ok(server.conn, "BACKUP", "ABORT")
    status = backup_status(server.conn)
    if status["state"] != "failed" or "aborted" not in status["error"]:
        raise FuzzFailure(f"abort did not produce failed state: {status!r}")
    if backup_files(server.conn):
        raise FuzzFailure("aborted backup retained pinned artifacts")
    validate_backup_directory_empty(server, "ABORT")
    clean_and_validate_idle(server)


def mode_pending(server: RedisProcess, rng: random.Random) -> None:
    seed_rewrite_dataset(server.conn, rng, "pending-rewrite")
    expect_ok(server.conn, "CONFIG", "SET", "rdb-key-save-delay", "100000")
    try:
        reply = server.conn.command("BGSAVE")
        if as_text(reply) not in (
            "Background saving started",
            "Background saving scheduled",
        ):
            raise FuzzFailure(f"unexpected BGSAVE reply {reply!r}")
        wait_for(
            server,
            lambda: info_field(
                server.conn, "persistence", "rdb_bgsave_in_progress"
            )
            == "1",
            "BGSAVE child",
        )
        start_backup(server)
        status = backup_status(server.conn)
        if status["state"] != "pending" or backup_files(server.conn):
            raise FuzzFailure(f"backup did not remain pending: {status!r}")
    finally:
        expect_ok(server.conn, "CONFIG", "SET", "rdb-key-save-delay", "0")
    validate_incrementing(server)
    seal_and_validate(server, rng)
    clean_and_validate_idle(server)


def mode_kill_rewrite(server: RedisProcess, rng: random.Random) -> None:
    seed_rewrite_dataset(server.conn, rng, "killed-rewrite")
    expect_ok(server.conn, "CONFIG", "SET", "rdb-key-save-delay", "100000")
    try:
        start_backup(server)
        wait_for_state(server, {"snapshotting"})

        def rewrite_child() -> int | None:
            if info_field(
                server.conn, "persistence", "aof_rewrite_in_progress"
            ) != "1":
                return None
            children = server.children()
            if len(children) > 1:
                raise FuzzFailure(
                    f"expected one AOF rewrite child, found {children!r}"
                )
            return children[0] if children else None

        child = wait_for(
            server,
            rewrite_child,
            "AOF rewrite child PID",
        )
        try:
            os.kill(child, signal.SIGKILL)
        except ProcessLookupError:
            # A very fast rewrite can exit after /proc is read. Only retry
            # while Redis still reports that a rewrite is in progress.
            child = wait_for(
                server,
                rewrite_child,
                "replacement AOF rewrite child PID",
                timeout=2.0,
            )
            os.kill(child, signal.SIGKILL)
    finally:
        expect_ok(server.conn, "CONFIG", "SET", "rdb-key-save-delay", "0")
    status = wait_for_state(server, {"failed"})
    if not status["error"]:
        raise FuzzFailure(f"killed rewrite produced no backup error: {status!r}")
    validate_backup_directory_empty(server, "killed rewrite")
    clean_and_validate_idle(server)


def mode_unlink_incr(server: RedisProcess, rng: random.Random) -> None:
    start_backup(server)
    validate_incrementing(server)
    mutate_dataset(server.conn, rng, 2)
    incr_file = active_live_incr(server)
    incr_file.unlink()
    expect_error(server.conn, "BACKUP", "SEAL", contains="can't pin")
    status = backup_status(server.conn)
    if status["state"] != "failed" or not status["error"]:
        raise FuzzFailure(f"missing INCR did not fail the backup: {status!r}")
    validate_backup_directory_empty(server, "missing INCR failure")
    clean_and_validate_idle(
        server,
        # This fault deliberately unlinks the live AOF file when appendonly
        # started enabled. Redis can keep writing through its open descriptor,
        # but the pathname cannot be required to exist after the injected fault.
        validate_live_aof=not server.initial_appendonly,
    )


def mode_nonempty_dir(server: RedisProcess, rng: random.Random) -> None:
    backup_dir = server.root / "redis-backupdir"
    backup_dir.mkdir(exist_ok=True)
    sentinel = backup_dir / f"foreign-{rng.randrange(1 << 20)}"
    sentinel.write_bytes(rng.randbytes(32))
    expected = sentinel.read_bytes()
    expect_error(server.conn, "BACKUP", "START", contains="not empty")
    if sentinel.read_bytes() != expected:
        raise FuzzFailure("BACKUP START modified a foreign backup-directory file")
    status = backup_status(server.conn)
    if status["state"] != "idle":
        raise FuzzFailure(f"rejected BACKUP START changed state: {status!r}")


def mode_transaction(
    server: RedisProcess,
    executable: Path,
    rng: random.Random,
    rdb_preamble: bool,
) -> None:
    before = rng.randbytes(64)
    after = rng.randbytes(64)
    if server.conn.command("MULTI") != "OK":
        raise FuzzFailure("MULTI failed")
    if server.conn.command("SET", "transaction-before", before) != "QUEUED":
        raise FuzzFailure("SET before BACKUP START was not queued")
    if server.conn.command("BACKUP", "START") != "QUEUED":
        raise FuzzFailure("BACKUP START was not queued")
    if server.conn.command("SET", "transaction-after", after) != "QUEUED":
        raise FuzzFailure("SET after BACKUP START was not queued")
    replies = server.conn.command("EXEC")
    if replies != ["OK", "OK", "OK"]:
        raise FuzzFailure(f"unexpected EXEC reply {replies!r}")
    base = validate_incrementing(server)
    verify_preload(
        executable,
        server,
        base,
        {
            "transaction-before": before,
            "transaction-after": after,
        },
        (),
        appendonly=False,
        rdb_preamble=rdb_preamble,
        label="transaction-base",
        preload_type="rdb" if base.name.endswith(".rdb") else "aof",
    )
    files = seal_and_validate(server, rng)
    if not files[0].is_file():
        raise FuzzFailure("transactional backup BASE disappeared")
    clean_and_validate_idle(server)


def mode_aof_flip(
    server: RedisProcess,
    rng: random.Random,
    appendonly: bool,
) -> None:
    start_backup(server)
    validate_incrementing(server)
    mutate_dataset(server.conn, rng, 3)
    if appendonly:
        expect_ok(server.conn, "CONFIG", "SET", "appendonly", "no")
        status = backup_status(server.conn)
        if (
            status["state"] != "failed"
            or "appendonly is stopped" not in status["error"]
        ):
            raise FuzzFailure(f"disabling AOF did not fail backup: {status!r}")
        validate_backup_directory_empty(server, "appendonly disable failure")
        clean_and_validate_idle(server)
    else:
        expect_ok(server.conn, "CONFIG", "SET", "appendonly", "yes")
        seal_and_validate(server, rng)
        config = server.conn.command("CONFIG", "GET", "appendonly")
        if not isinstance(config, list) or as_text(config[-1]) != "yes":
            raise FuzzFailure(f"AOF did not remain enabled after seal: {config!r}")
        clean_and_validate_idle(server)


def invalid_transition_burst(conn: RespConnection, rng: random.Random) -> None:
    commands = [
        ("BACKUP",),
        ("BACKUP", "SEAL"),
        ("BACKUP", "ABORT"),
        ("BACKUP", "START", "extra"),
        ("BACKUP", "UNKNOWN"),
    ]
    rng.shuffle(commands)
    for command in commands[: 1 + rng.randrange(len(commands))]:
        expect_error(conn, *command)
    reply = conn.command("BACKUP", "HELP")
    if not isinstance(reply, list) or not reply:
        raise FuzzFailure(f"unexpected BACKUP HELP reply {reply!r}")


def execute_scenario(seed: int, executable: Path, root: Path) -> str:
    rng = random.Random(seed)
    mode = seed & 7
    appendonly = bool((seed >> 3) & 1)
    rdb_preamble = bool((seed >> 4) & 1)
    server: RedisProcess | None = None
    try:
        server = RedisProcess(
            executable,
            root,
            appendonly=appendonly,
            rdb_preamble=rdb_preamble,
        )
        mutate_dataset(server.conn, rng, 1 + rng.randrange(12))
        invalid_transition_burst(server.conn, rng)

        modes = (
            lambda: mode_full(server, executable, rng, rdb_preamble),
            lambda: mode_abort(server, rng),
            lambda: mode_pending(server, rng),
            lambda: mode_kill_rewrite(server, rng),
            lambda: mode_unlink_incr(server, rng),
            lambda: mode_nonempty_dir(server, rng),
            lambda: mode_transaction(server, executable, rng, rdb_preamble),
            lambda: mode_aof_flip(server, rng, appendonly),
        )
        modes[mode]()
        server.assert_alive()
        server.check_log()
        summary = (
            f"mode={mode} appendonly={'yes' if appendonly else 'no'} "
            f"rdb_preamble={'yes' if rdb_preamble else 'no'}"
        )
        return summary
    finally:
        if server is not None:
            server.stop()


def read_seeds(path: Path) -> list[int]:
    seeds: list[int] = []
    for line in path.read_text().splitlines():
        stripped = line.split("#", 1)[0].strip()
        if stripped:
            seeds.append(int(stripped, 0))
    if not seeds:
        raise FuzzFailure(f"seed corpus {path} is empty")
    return seeds


def copy_scenario_artifacts(source: Path, destination: Path) -> list[str]:
    skipped: list[str] = []

    def ignore_special_files(directory: str, names: list[str]) -> list[str]:
        ignored: list[str] = []
        for name in names:
            path = Path(directory) / name
            try:
                mode = path.lstat().st_mode
            except OSError as exc:
                ignored.append(name)
                skipped.append(f"{path}: lstat failed: {exc}")
                continue
            if not (
                stat.S_ISREG(mode)
                or stat.S_ISDIR(mode)
                or stat.S_ISLNK(mode)
            ):
                ignored.append(name)
                skipped.append(f"{path}: special file mode {mode:o}")
        return ignored

    shutil.copytree(
        source,
        destination,
        symlinks=True,
        ignore=ignore_special_files,
    )
    return skipped


def preserve_failure(
    artifact_dir: Path,
    root: Path | None,
    seed: int,
    error: BaseException,
) -> Path:
    base = artifact_dir / f"failure-{seed:016x}"
    destination = base
    suffix = 1
    while destination.exists():
        destination = artifact_dir / f"{base.name}-{suffix}"
        suffix += 1
    destination.mkdir(parents=True)
    metadata = {
        "seed": seed,
        "seed_hex": f"0x{seed:016x}",
        "error": str(error),
        "traceback": traceback.format_exc(),
        "reproduce": (
            "python3 fuzz/fuzz_backup_state_machine.py "
            f"--redis-server src/redis-server --seed 0x{seed:016x}"
        ),
    }
    (destination / "reproducer.json").write_text(
        json.dumps(metadata, indent=2) + "\n"
    )
    if root is not None and root.exists():
        skipped = copy_scenario_artifacts(root, destination / "scenario")
        if skipped:
            (destination / "skipped-special-files.txt").write_text(
                "\n".join(skipped) + "\n"
            )
    return destination


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-server", type=Path, required=True)
    parser.add_argument(
        "--seeds",
        type=Path,
        default=Path("fuzz/corpus/backup_state_machine/seeds.txt"),
    )
    parser.add_argument("--seed", type=lambda value: int(value, 0))
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--max-runs", type=int, default=0)
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        default=Path("fuzz-backup-artifacts"),
    )
    args = parser.parse_args()

    executable = args.redis_server.resolve()
    if not executable.is_file():
        parser.error(f"Redis server not found: {executable}")
    args.artifact_dir.mkdir(parents=True, exist_ok=True)

    if args.seed is not None:
        seeds = [args.seed]
        run_limit = args.max_runs or 1
    else:
        seeds = read_seeds(args.seeds)
        run_limit = args.max_runs
    seed_source = random.Random(0xBACC0FFEE)
    start = time.monotonic()
    completed = 0

    while True:
        if run_limit and completed >= run_limit:
            break
        if completed >= len(seeds) and time.monotonic() - start >= args.duration:
            break
        seed = (
            seeds[completed]
            if completed < len(seeds)
            else seed_source.getrandbits(64)
        )
        root = Path(tempfile.mkdtemp(prefix=f"redis-backup-fuzz-{seed:016x}-"))
        remove_root = True
        try:
            summary = execute_scenario(seed, executable, root)
            completed += 1
            elapsed = time.monotonic() - start
            print(
                f"PASS seed=0x{seed:016x} {summary} "
                f"runs={completed} elapsed={elapsed:.2f}s",
                flush=True,
            )
        except BaseException as exc:
            try:
                destination = preserve_failure(
                    args.artifact_dir, root, seed, exc
                )
                artifact_message = f" artifacts={destination}"
            except BaseException as preservation_error:
                # Keep the original temporary directory as the last-resort
                # evidence rather than deleting it after a copy failure.
                remove_root = False
                artifact_message = (
                    f" artifact-preservation-error={preservation_error!r} "
                    f"scenario-left-at={root}"
                )
            print(
                f"FAIL seed=0x{seed:016x}: {exc}{artifact_message}",
                file=sys.stderr,
                flush=True,
            )
            return 1
        finally:
            if remove_root:
                shutil.rmtree(root, ignore_errors=True)

    elapsed = time.monotonic() - start
    print(
        f"DONE runs={completed} elapsed={elapsed:.2f}s "
        f"artifacts={args.artifact_dir}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
