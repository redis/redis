#!/usr/bin/env python3
"""Streaming redis-roaring to native Redis bitmap migrator.

The tool intentionally lives outside Redis core. It reads redis-roaring module
keys through bounded integer-array commands, builds target-native bitmap values
with the target Redis encoder, validates temporary keys, and then atomically
renames them into place.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator, Optional


NATIVE_V1_MAX_BIT = 4294967295
MANIFEST_VERSION = 1


class MigrateError(RuntimeError):
    pass


class RedisError(MigrateError):
    pass


def encode_resp(parts: list[Any]) -> bytes:
    out = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        if isinstance(part, bytes):
            data = part
        else:
            data = str(part).encode("utf-8")
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)


def decode_text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def parse_uint(value: Any, field: str) -> int:
    if isinstance(value, int):
        if value < 0:
            raise MigrateError(f"{field} is negative: {value}")
        return value
    text = decode_text(value).strip()
    if not text.isdigit():
        raise MigrateError(f"{field} is not an unsigned integer: {text!r}")
    return int(text)


def parse_offset_or_minus_one(value: Any, field: str) -> int:
    if isinstance(value, int):
        if value < -1:
            raise MigrateError(f"{field} is less than -1: {value}")
        return value
    text = decode_text(value).strip()
    if text == "-1":
        return -1
    if not text.isdigit():
        raise MigrateError(f"{field} is not an offset or -1: {text!r}")
    return int(text)


def is_wrongtype_error(exc: RedisError) -> bool:
    message = str(exc).upper()
    return "WRONGTYPE" in message or "OPERATION AGAINST A KEY" in message


def is_unknown_command_error(exc: RedisError) -> bool:
    message = str(exc).upper()
    return "UNKNOWN COMMAND" in message or "ERR UNKNOWN" in message


def key_to_text(key: bytes) -> str:
    return key.decode("utf-8", "replace")


def key_to_b64(key: bytes) -> str:
    return base64.b64encode(key).decode("ascii")


def now_ms() -> int:
    return int(time.time() * 1000)


@dataclass(frozen=True)
class RedisEndpoint:
    host: str
    port: int
    db: int
    user: Optional[str]
    password: Optional[str]
    timeout: float
    name: str

    def with_db(self, db: int) -> "RedisEndpoint":
        return RedisEndpoint(
            host=self.host,
            port=self.port,
            db=db,
            user=self.user,
            password=self.password,
            timeout=self.timeout,
            name=self.name,
        )


class RespConnection:
    def __init__(self, endpoint: RedisEndpoint):
        self.endpoint = endpoint
        self.sock: Optional[socket.socket] = None
        self.fp: Any = None

    def __enter__(self) -> "RespConnection":
        self.sock = socket.create_connection((self.endpoint.host, self.endpoint.port), timeout=self.endpoint.timeout)
        self.sock.settimeout(self.endpoint.timeout)
        self.fp = self.sock.makefile("rb")
        if self.endpoint.password:
            if self.endpoint.user:
                self.command(["AUTH", self.endpoint.user, self.endpoint.password])
            else:
                self.command(["AUTH", self.endpoint.password])
        if self.endpoint.db:
            self.command(["SELECT", str(self.endpoint.db)])
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        if self.fp is not None:
            self.fp.close()
        if self.sock is not None:
            self.sock.close()

    def command(self, parts: list[Any]) -> Any:
        if self.sock is None:
            raise MigrateError("RESP connection is not open")
        self.sock.sendall(encode_resp(parts))
        return self.read_response()

    def send_pipeline(self, commands: list[list[Any]]) -> list[Any]:
        if not commands:
            return []
        if self.sock is None:
            raise MigrateError("RESP connection is not open")
        self.sock.sendall(b"".join(encode_resp(cmd) for cmd in commands))
        return [self.read_response() for _ in commands]

    def read_response(self) -> Any:
        if self.fp is None:
            raise MigrateError("RESP connection is not open")
        prefix = self.fp.read(1)
        if not prefix:
            raise RedisError(f"{self.endpoint.name}: redis closed the connection")
        if prefix == b"+":
            return self._read_line().decode("utf-8", "replace")
        if prefix == b"-":
            raise RedisError(self._read_line().decode("utf-8", "replace"))
        if prefix == b":":
            return int(self._read_line())
        if prefix == b",":
            return float(self._read_line())
        if prefix == b"#":
            raw = self._read_line()
            return raw == b"t"
        if prefix == b"_":
            self._read_line()
            return None
        if prefix == b"$":
            return self._read_bulk()
        if prefix == b"=":
            return self._read_bulk()
        if prefix == b"*":
            return self._read_array()
        if prefix == b"%":
            length = int(self._read_line())
            out: dict[Any, Any] = {}
            for _ in range(length):
                key = self.read_response()
                out[key] = self.read_response()
            return out
        raise RedisError(f"{self.endpoint.name}: unknown RESP prefix: {prefix!r}")

    def _read_line(self) -> bytes:
        if self.fp is None:
            raise MigrateError("RESP connection is not open")
        line = self.fp.readline()
        if not line.endswith(b"\r\n"):
            raise RedisError(f"{self.endpoint.name}: invalid RESP line")
        return line[:-2]

    def _read_bulk(self) -> Optional[bytes]:
        length = int(self._read_line())
        if length == -1:
            return None
        data = self.fp.read(length)
        crlf = self.fp.read(2)
        if crlf != b"\r\n":
            raise RedisError(f"{self.endpoint.name}: invalid bulk string terminator")
        return data

    def _read_array(self) -> Optional[list[Any]]:
        length = int(self._read_line())
        if length == -1:
            return None
        return [self.read_response() for _ in range(length)]


class RedisClient:
    def __init__(self, endpoint: RedisEndpoint):
        self.endpoint = endpoint

    def with_db(self, db: int) -> "RedisClient":
        return RedisClient(self.endpoint.with_db(db))

    def connection(self) -> RespConnection:
        return RespConnection(self.endpoint)

    def execute(self, command: list[Any]) -> Any:
        with self.connection() as conn:
            return conn.command(command)


@dataclass
class SourceInfo:
    db: int
    key: bytes
    source_type: str
    command_prefix: str
    cardinality: int
    min_offset: int
    max_offset: int
    expire_at_ms: Optional[int]
    ttl_ms: int
    source_hash: Optional[str]


class Manifest:
    def __init__(self, path: Path, args: argparse.Namespace):
        self.path = path
        self.data: dict[str, Any] = {
            "version": MANIFEST_VERSION,
            "created_at_ms": now_ms(),
            "updated_at_ms": now_ms(),
            "tool": "redis-bitmap-migrate",
            "options": self._safe_options(args),
            "entries": [],
        }
        self.entries_by_id: dict[str, dict[str, Any]] = {}

    @staticmethod
    def _safe_options(args: argparse.Namespace) -> dict[str, Any]:
        hidden = {"source_password", "target_password"}
        result: dict[str, Any] = {}
        for key, value in vars(args).items():
            if key in hidden and value:
                result[key] = "<redacted>"
            elif isinstance(value, Path):
                result[key] = str(value)
            else:
                result[key] = value
        return result

    @classmethod
    def load_or_create(cls, path: Path, args: argparse.Namespace, resume: bool) -> "Manifest":
        manifest = cls(path, args)
        if path.exists():
            if not resume:
                raise MigrateError(f"manifest already exists: {path} (use --resume or choose another path)")
            with path.open("r", encoding="utf-8") as fp:
                manifest.data = json.load(fp)
            if manifest.data.get("version") != MANIFEST_VERSION:
                raise MigrateError(f"unsupported manifest version in {path}")
        for entry in manifest.data.get("entries", []):
            entry_id = entry.get("id")
            if entry_id:
                manifest.entries_by_id[entry_id] = entry
        return manifest

    def get(self, entry_id: str) -> Optional[dict[str, Any]]:
        return self.entries_by_id.get(entry_id)

    def upsert(self, entry: dict[str, Any]) -> None:
        entry["updated_at_ms"] = now_ms()
        entry_id = entry["id"]
        existing = self.entries_by_id.get(entry_id)
        if existing is None:
            self.data.setdefault("entries", []).append(entry)
            self.entries_by_id[entry_id] = entry
        elif existing is entry:
            pass
        else:
            existing.clear()
            existing.update(entry)
        self.data["updated_at_ms"] = now_ms()
        self.save()

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=f".{self.path.name}.", suffix=".tmp", dir=str(self.path.parent))
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fp:
                json.dump(self.data, fp, indent=2, sort_keys=True)
                fp.write("\n")
            os.replace(tmp, self.path)
        except Exception:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise


class BitmapMigrator:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.source = RedisClient(
            RedisEndpoint(
                args.source_host,
                args.source_port,
                0,
                args.source_user,
                args.source_password,
                args.socket_timeout,
                "source",
            )
        )
        self.target = RedisClient(
            RedisEndpoint(
                args.target_host,
                args.target_port,
                0,
                args.target_user,
                args.target_password,
                args.socket_timeout,
                "target",
            )
        )
        self.manifest = Manifest.load_or_create(Path(args.manifest), args, args.resume)
        self.summary = {
            "scanned": 0,
            "non_roaring": 0,
            "planned": 0,
            "migrated": 0,
            "skipped": 0,
            "failed": 0,
            "resumed": 0,
        }

    def run(self) -> int:
        self.manifest.save()
        self.preflight()
        dbs = self.discover_dbs()
        for db in dbs:
            self.migrate_db(db)
        self.print_summary()
        return 1 if self.summary["failed"] else 0

    def preflight(self) -> None:
        self.source.execute(["PING"])
        self.target.execute(["PING"])
        if self.args.apply and not self.args.assume_frozen and not self.args.allow_live_copy:
            raise MigrateError("--apply requires --assume-frozen or --allow-live-copy")
        if self.cluster_enabled(self.source) and not self.args.allow_cluster:
            raise MigrateError("source cluster mode is enabled; this tool does not migrate cluster targets unless --allow-cluster is set")
        if self.cluster_enabled(self.target) and not self.args.allow_cluster:
            raise MigrateError("target cluster mode is enabled; this tool does not migrate cluster targets unless --allow-cluster is set")
        if self.args.allow_cluster:
            raise MigrateError("cluster migration is not implemented yet; rerun against standalone masters or omit --allow-cluster")

    def cluster_enabled(self, client: RedisClient) -> bool:
        try:
            raw = decode_text(client.execute(["INFO", "cluster"]))
        except RedisError:
            return False
        for line in raw.splitlines():
            if line.startswith("cluster_enabled:"):
                return line.split(":", 1)[1].strip() == "1"
        return False

    def discover_dbs(self) -> list[int]:
        if self.args.all_dbs:
            raw = decode_text(self.source.execute(["INFO", "keyspace"]))
            dbs: list[int] = []
            for line in raw.splitlines():
                if not line.startswith("db") or ":" not in line:
                    continue
                name = line.split(":", 1)[0]
                if name[2:].isdigit():
                    dbs.append(int(name[2:]))
            return sorted(dbs) or [0]
        return sorted(set(self.args.db))

    def migrate_db(self, db: int) -> None:
        source = self.source.with_db(db)
        target = self.target.with_db(db)
        for key in self.iter_keys(source):
            self.summary["scanned"] += 1
            try:
                self.migrate_key(source, target, db, key)
            except RedisError as exc:
                self.record_failure(db, key, str(exc))
                if not self.args.keep_going:
                    raise
            except MigrateError as exc:
                self.record_failure(db, key, str(exc))
                if not self.args.keep_going:
                    raise

    def iter_keys(self, source: RedisClient) -> Iterator[bytes]:
        if self.args.key:
            for key in self.args.key:
                yield key.encode("utf-8")
            return
        if self.args.key_file:
            with open(self.args.key_file, "rb") as fp:
                for raw in fp:
                    key = raw.rstrip(b"\r\n")
                    if key:
                        yield key
            return

        cursor = 0
        with source.connection() as conn:
            while True:
                command: list[Any] = ["SCAN", str(cursor), "COUNT", str(self.args.scan_count)]
                if self.args.pattern:
                    command.extend(["MATCH", self.args.pattern])
                reply = conn.command(command)
                if not isinstance(reply, list) or len(reply) != 2:
                    raise MigrateError(f"unexpected SCAN reply: {reply!r}")
                cursor = parse_uint(reply[0], "SCAN cursor")
                keys = reply[1] or []
                for key in keys:
                    if isinstance(key, bytes):
                        yield key
                    else:
                        yield str(key).encode("utf-8")
                if cursor == 0:
                    break

    def migrate_key(self, source: RedisClient, target: RedisClient, db: int, key: bytes) -> None:
        entry_id = self.entry_id(db, key)
        existing = self.manifest.get(entry_id)
        if existing and existing.get("state") in {"committed", "skipped"}:
            self.summary["resumed"] += 1
            return
        if existing and existing.get("state") == "dry_run" and self.args.dry_run:
            self.summary["resumed"] += 1
            return

        info = self.detect_source(source, db, key)
        if info is None:
            if self.args.key or self.args.key_file or self.args.strict_scan:
                raise MigrateError("key is not a redis-roaring bitmap")
            self.summary["non_roaring"] += 1
            return

        dest_key = self.destination_key(key)
        build_key = self.manifest_key(db, key, b"build")
        temp_key = self.manifest_key(db, key, b"tmp")
        entry = self.base_entry(info, dest_key, build_key, temp_key)

        if info.max_offset > NATIVE_V1_MAX_BIT:
            error = (
                f"source max offset {info.max_offset} exceeds v1 native bitmap cap "
                f"{NATIVE_V1_MAX_BIT}"
            )
            entry["error"] = error
            if self.args.cap_policy == "skip":
                entry["state"] = "skipped"
                self.summary["skipped"] += 1
                self.manifest.upsert(entry)
                print(f"SKIP db={db} key={key_to_text(key)}: {error}", file=sys.stderr)
                return
            entry["state"] = "failed"
            self.manifest.upsert(entry)
            raise MigrateError(error)

        if self.args.dry_run:
            entry["state"] = "dry_run"
            self.summary["planned"] += 1
            self.manifest.upsert(entry)
            print(f"DRY-RUN db={db} key={key_to_text(key)} type={info.source_type} count={info.cardinality}")
            return

        if existing and self.args.resume:
            if self.resume_imported_key(target, info, existing, dest_key, temp_key):
                return

        entry["state"] = "exporting"
        self.manifest.upsert(entry)
        samples, full_offsets = self.import_to_temp(source, target, info, build_key, temp_key)
        entry["sample_offsets"] = sorted(samples)
        entry["full_diff_offsets"] = sorted(full_offsets) if full_offsets is not None else None
        entry["state"] = "imported"
        self.manifest.upsert(entry)

        validation = self.validate_temp(target, info, temp_key, samples, full_offsets)
        entry["validation"] = validation
        entry["state"] = "validated"
        self.manifest.upsert(entry)

        self.commit_temp(target, dest_key, temp_key)
        entry["state"] = "committed"
        entry["committed_at_ms"] = now_ms()
        entry["error"] = None
        self.manifest.upsert(entry)
        self.summary["migrated"] += 1
        print(f"MIGRATED db={db} key={key_to_text(key)} count={info.cardinality}")

    def resume_imported_key(
        self,
        target: RedisClient,
        info: SourceInfo,
        entry: dict[str, Any],
        dest_key: bytes,
        temp_key: bytes,
    ) -> bool:
        state = entry.get("state")
        if state not in {"imported", "validated", "committing"}:
            return False
        sample_offsets = {int(offset) for offset in entry.get("sample_offsets") or []}
        full_raw = entry.get("full_diff_offsets")
        full_offsets = None if full_raw is None else {int(offset) for offset in full_raw}

        if parse_uint(target.execute(["EXISTS", temp_key]), "EXISTS temp") == 1:
            validation = self.validate_temp(target, info, temp_key, sample_offsets, full_offsets)
            entry["validation"] = validation
            entry["state"] = "validated"
            self.manifest.upsert(entry)
            entry["state"] = "committing"
            self.manifest.upsert(entry)
            self.commit_temp(target, dest_key, temp_key)
            entry["state"] = "committed"
            entry["committed_at_ms"] = now_ms()
            entry["error"] = None
            self.manifest.upsert(entry)
            self.summary["migrated"] += 1
            print(f"RESUMED db={info.db} key={key_to_text(info.key)} from temporary key")
            return True

        if parse_uint(target.execute(["EXISTS", dest_key]), "EXISTS destination") == 1:
            validation = self.validate_temp(target, info, dest_key, sample_offsets, full_offsets)
            entry["validation"] = validation
            entry["state"] = "committed"
            entry["committed_at_ms"] = now_ms()
            entry["error"] = None
            self.manifest.upsert(entry)
            self.summary["resumed"] += 1
            print(f"RESUMED db={info.db} key={key_to_text(info.key)} from destination key")
            return True

        return False

    def detect_source(self, source: RedisClient, db: int, key: bytes) -> Optional[SourceInfo]:
        if parse_uint(source.execute(["EXISTS", key]), "EXISTS") == 0:
            return None
        candidates = []
        if self.args.source_type in {"auto", "reroaring"}:
            candidates.append(("reroaring", "R"))
        if self.args.source_type in {"auto", "roaring64"}:
            candidates.append(("roaring64", "R64"))

        last_wrongtype: Optional[RedisError] = None
        for source_type, prefix in candidates:
            try:
                cardinality = parse_uint(source.execute([f"{prefix}.BITCOUNT", key]), "cardinality")
                min_offset = parse_offset_or_minus_one(source.execute([f"{prefix}.MIN", key]), "min offset")
                max_offset = parse_offset_or_minus_one(source.execute([f"{prefix}.MAX", key]), "max offset")
                expire_at_ms, ttl_ms = self.read_expire(source, key)
                if ttl_ms == -2:
                    return None
                source_hash = self.source_payload_hash(source, key)
                return SourceInfo(
                    db=db,
                    key=key,
                    source_type=source_type,
                    command_prefix=prefix,
                    cardinality=cardinality,
                    min_offset=min_offset,
                    max_offset=max_offset,
                    expire_at_ms=expire_at_ms,
                    ttl_ms=ttl_ms,
                    source_hash=source_hash,
                )
            except RedisError as exc:
                if is_wrongtype_error(exc):
                    last_wrongtype = exc
                    continue
                if is_unknown_command_error(exc):
                    raise MigrateError(f"source redis-roaring command {prefix}.BITCOUNT is unavailable: {exc}")
                raise
        if self.args.source_type != "auto" and last_wrongtype is not None:
            raise MigrateError(f"key is not {self.args.source_type}: {last_wrongtype}")
        return None

    def read_expire(self, source: RedisClient, key: bytes) -> tuple[Optional[int], int]:
        try:
            expire_at = int(source.execute(["PEXPIRETIME", key]))
            if expire_at > 0:
                return expire_at, max(expire_at - now_ms(), 0)
            return None, expire_at
        except RedisError as exc:
            if not is_unknown_command_error(exc):
                raise
        ttl = int(source.execute(["PTTL", key]))
        if ttl > 0:
            return now_ms() + ttl, ttl
        return None, ttl

    def source_payload_hash(self, source: RedisClient, key: bytes) -> Optional[str]:
        try:
            payload = source.execute(["DUMP", key])
        except RedisError:
            return None
        if payload is None:
            return None
        if not isinstance(payload, bytes):
            payload = str(payload).encode("utf-8")
        return "sha256:" + hashlib.sha256(payload).hexdigest()

    def import_to_temp(
        self,
        source: RedisClient,
        target: RedisClient,
        info: SourceInfo,
        build_key: bytes,
        temp_key: bytes,
    ) -> tuple[set[int], Optional[set[int]]]:
        sample_ranks = self.sample_ranks(info.cardinality)
        sample_offsets: set[int] = set()
        full_offsets: Optional[set[int]] = set() if info.cardinality <= self.args.full_diff_limit else None
        exported_count = 0
        last_offset: Optional[int] = None

        with source.connection() as src, target.connection() as dst:
            dst.command(["DEL", build_key, temp_key])
            if info.cardinality == 0:
                dst.command(["SET", build_key, b""])
            else:
                dst.command(["SETBIT", build_key, "0", "0"])
            dst.command(["BITMAP", "CONVERT", build_key, "NATIVE"])

            if info.cardinality:
                for rank_start in range(0, info.cardinality, self.args.page_size):
                    rank_end = min(info.cardinality - 1, rank_start + self.args.page_size - 1)
                    raw_offsets = src.command([
                        f"{info.command_prefix}.RANGEINTARRAY",
                        info.key,
                        str(rank_start),
                        str(rank_end),
                    ])
                    if not isinstance(raw_offsets, list):
                        raise MigrateError(f"unexpected RANGEINTARRAY reply for {key_to_text(info.key)}")
                    expected = rank_end - rank_start + 1
                    if len(raw_offsets) != expected:
                        raise MigrateError(
                            f"source changed while exporting {key_to_text(info.key)}: "
                            f"expected {expected} offsets, got {len(raw_offsets)}"
                        )
                    offsets = [parse_uint(offset, "offset") for offset in raw_offsets]
                    commands: list[list[Any]] = []
                    for i, offset in enumerate(offsets):
                        if offset > NATIVE_V1_MAX_BIT:
                            raise MigrateError(f"offset {offset} exceeds v1 native bitmap cap")
                        if last_offset is not None and offset <= last_offset:
                            raise MigrateError(f"source offsets are not strictly increasing at {offset}")
                        absolute_rank = rank_start + i
                        if absolute_rank in sample_ranks:
                            sample_offsets.add(offset)
                        if full_offsets is not None:
                            full_offsets.add(offset)
                        commands.append(["SETBIT", build_key, str(offset), "1"])
                        last_offset = offset
                    self.send_in_chunks(dst, commands, self.args.pipeline_size)
                    exported_count += len(offsets)

            if exported_count != info.cardinality:
                raise MigrateError(
                    f"exported {exported_count} offsets from {key_to_text(info.key)}, "
                    f"expected {info.cardinality}"
                )

            current_count = parse_uint(src.command([f"{info.command_prefix}.BITCOUNT", info.key]), "cardinality")
            if current_count != info.cardinality:
                raise MigrateError(
                    f"source cardinality changed during export for {key_to_text(info.key)}: "
                    f"{info.cardinality} -> {current_count}"
                )

            payload = dst.command(["DUMP", build_key])
            if payload is None:
                raise MigrateError("target DUMP returned nil for build key")
            restore_cmd: list[Any]
            if info.expire_at_ms is not None:
                restore_cmd = ["RESTORE", temp_key, str(info.expire_at_ms), payload, "REPLACE", "ABSTTL"]
            else:
                restore_cmd = ["RESTORE", temp_key, "0", payload, "REPLACE"]
            dst.command(restore_cmd)
            dst.command(["DEL", build_key])

        if info.cardinality and not sample_offsets:
            sample_offsets.add(info.min_offset)
            sample_offsets.add(info.max_offset)
        return sample_offsets, full_offsets

    def send_in_chunks(self, conn: RespConnection, commands: list[list[Any]], chunk_size: int) -> None:
        for start in range(0, len(commands), chunk_size):
            conn.send_pipeline(commands[start:start + chunk_size])

    def sample_ranks(self, cardinality: int) -> set[int]:
        if cardinality <= 0:
            return set()
        count = min(cardinality, self.args.sample_count)
        if count == 1:
            return {0}
        return {round(i * (cardinality - 1) / (count - 1)) for i in range(count)}

    def validate_temp(
        self,
        target: RedisClient,
        info: SourceInfo,
        temp_key: bytes,
        sample_offsets: set[int],
        full_offsets: Optional[set[int]],
    ) -> dict[str, Any]:
        checks: dict[str, Any] = {}
        with target.connection() as conn:
            key_type = decode_text(conn.command(["TYPE", temp_key])).lower()
            checks["type"] = key_type
            if key_type != "bitmap":
                raise MigrateError(f"temporary key type is {key_type!r}, expected 'bitmap'")
            encoding = decode_text(conn.command(["OBJECT", "ENCODING", temp_key])).lower()
            checks["encoding"] = encoding
            if "bitmap" not in encoding:
                raise MigrateError(f"temporary key encoding is {encoding!r}, expected bitmap encoding")
            cardinality = parse_uint(conn.command(["BITCOUNT", temp_key]), "target cardinality")
            checks["cardinality"] = cardinality
            if cardinality != info.cardinality:
                raise MigrateError(f"target cardinality {cardinality} != source {info.cardinality}")
            bitpos = int(conn.command(["BITPOS", temp_key, "1"]))
            checks["bitpos_1"] = bitpos
            expected_min = -1 if info.cardinality == 0 else info.min_offset
            if bitpos != expected_min:
                raise MigrateError(f"target BITPOS 1 {bitpos} != source min {expected_min}")
            if info.cardinality == 0:
                bitpos_zero = int(conn.command(["BITPOS", temp_key, "0"]))
                checks["bitpos_0"] = bitpos_zero
                if bitpos_zero != -1:
                    raise MigrateError(f"target BITPOS 0 {bitpos_zero} != empty native bitmap -1")
            else:
                if parse_uint(conn.command(["GETBIT", temp_key, str(info.min_offset)]), "GETBIT min") != 1:
                    raise MigrateError("target min offset is not set")
                if parse_uint(conn.command(["GETBIT", temp_key, str(info.max_offset)]), "GETBIT max") != 1:
                    raise MigrateError("target max offset is not set")
                if info.min_offset > 0:
                    before_min = parse_uint(conn.command(["GETBIT", temp_key, str(info.min_offset - 1)]), "GETBIT before min")
                    if before_min != 0:
                        raise MigrateError("target has a bit set before source min")
                if info.max_offset < NATIVE_V1_MAX_BIT:
                    after_max = parse_uint(conn.command(["GETBIT", temp_key, str(info.max_offset + 1)]), "GETBIT after max")
                    if after_max != 0:
                        raise MigrateError("target has a bit set after source max")
            for offset in sorted(sample_offsets):
                if parse_uint(conn.command(["GETBIT", temp_key, str(offset)]), "GETBIT sample") != 1:
                    raise MigrateError(f"target sampled offset {offset} is not set")
            if full_offsets is not None:
                for offset in sorted(full_offsets):
                    if parse_uint(conn.command(["GETBIT", temp_key, str(offset)]), "GETBIT diff") != 1:
                        raise MigrateError(f"target offset {offset} is missing from full diff check")
                if len(full_offsets) != cardinality:
                    raise MigrateError("full diff offset count does not match target cardinality")
            target_ttl = int(conn.command(["PTTL", temp_key]))
            checks["ttl_ms"] = target_ttl
            if info.expire_at_ms is not None and target_ttl < 0:
                raise MigrateError("temporary key lost source TTL")
        return checks

    def commit_temp(self, target: RedisClient, dest_key: bytes, temp_key: bytes) -> None:
        with target.connection() as conn:
            exists = parse_uint(conn.command(["EXISTS", dest_key]), "EXISTS destination")
            if exists and not self.args.replace:
                conn.command(["DEL", temp_key])
                raise MigrateError(f"destination key already exists: {key_to_text(dest_key)} (use --replace)")
            if parse_uint(conn.command(["EXISTS", temp_key]), "EXISTS temp") != 1:
                raise MigrateError(f"temporary key is missing before commit: {key_to_text(temp_key)}")
            if self.args.replace:
                conn.command(["RENAME", temp_key, dest_key])
                return
            renamed = parse_uint(conn.command(["RENAMENX", temp_key, dest_key]), "RENAMENX")
            if renamed != 1:
                conn.command(["DEL", temp_key])
                raise MigrateError(f"destination key already exists: {key_to_text(dest_key)}")

    def record_failure(self, db: int, key: bytes, error: str) -> None:
        entry_id = self.entry_id(db, key)
        entry = self.manifest.get(entry_id) or {
            "id": entry_id,
            "db": db,
            "key": key_to_text(key),
            "key_b64": key_to_b64(key),
        }
        already_failed = entry.get("state") == "failed"
        entry["state"] = "failed"
        entry["error"] = error
        if not already_failed:
            self.summary["failed"] += 1
        self.manifest.upsert(entry)
        print(f"FAILED db={db} key={key_to_text(key)}: {error}", file=sys.stderr)

    def base_entry(self, info: SourceInfo, dest_key: bytes, build_key: bytes, temp_key: bytes) -> dict[str, Any]:
        return {
            "id": self.entry_id(info.db, info.key),
            "state": "planned",
            "db": info.db,
            "key": key_to_text(info.key),
            "key_b64": key_to_b64(info.key),
            "destination_key": key_to_text(dest_key),
            "destination_key_b64": key_to_b64(dest_key),
            "temporary_key": key_to_text(temp_key),
            "temporary_key_b64": key_to_b64(temp_key),
            "build_key": key_to_text(build_key),
            "build_key_b64": key_to_b64(build_key),
            "source_type": info.source_type,
            "ttl_ms": info.ttl_ms,
            "expire_at_ms": info.expire_at_ms,
            "cardinality": info.cardinality,
            "min_offset": info.min_offset,
            "max_offset": info.max_offset,
            "max_observed_offset": info.max_offset,
            "source_hash": info.source_hash,
            "overwrite_policy": "replace" if self.args.replace else "no-overwrite",
            "cap_policy": self.args.cap_policy,
            "error": None,
        }

    def destination_key(self, key: bytes) -> bytes:
        return self.args.target_prefix.encode("utf-8") + key + self.args.target_suffix.encode("utf-8")

    def manifest_key(self, db: int, key: bytes, suffix: bytes) -> bytes:
        digest = hashlib.sha256(str(db).encode("ascii") + b":" + key).hexdigest().encode("ascii")
        return b"__redis-bitmap-migrate:" + digest + b":" + suffix

    def entry_id(self, db: int, key: bytes) -> str:
        digest = hashlib.sha256(key).hexdigest()
        return f"db{db}:{digest}"

    def print_summary(self) -> None:
        print(
            "summary: "
            f"scanned={self.summary['scanned']} "
            f"planned={self.summary['planned']} "
            f"migrated={self.summary['migrated']} "
            f"skipped={self.summary['skipped']} "
            f"non_roaring={self.summary['non_roaring']} "
            f"failed={self.summary['failed']} "
            f"resumed={self.summary['resumed']}"
        )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Migrate redis-roaring R.* / R64.* module keys into native Redis bitmap "
            "values using an external streaming workflow."
        )
    )
    parser.add_argument("--source-host", default="127.0.0.1")
    parser.add_argument("--source-port", type=int, default=6379)
    parser.add_argument("--source-user")
    parser.add_argument("--source-password")
    parser.add_argument("--target-host", default="127.0.0.1")
    parser.add_argument("--target-port", type=int, default=6380)
    parser.add_argument("--target-user")
    parser.add_argument("--target-password")
    parser.add_argument("--db", type=int, action="append", help="DB index to migrate; repeatable")
    parser.add_argument("--all-dbs", action="store_true", help="discover DBs from INFO keyspace")
    parser.add_argument("--pattern", default="*", help="SCAN MATCH pattern")
    parser.add_argument("--key", action="append", help="specific UTF-8 key to migrate; repeatable")
    parser.add_argument("--key-file", help="newline-delimited binary-safe key file")
    parser.add_argument("--source-type", choices=("auto", "reroaring", "roaring64"), default="auto")
    parser.add_argument("--target-prefix", default="", help="prefix to add to destination keys")
    parser.add_argument("--target-suffix", default="", help="suffix to add to destination keys")
    parser.add_argument("--manifest", default="redis-bitmap-migrate-manifest.json")
    parser.add_argument("--resume", action="store_true", help="resume from an existing manifest")
    parser.add_argument("--apply", action="store_true", help="perform writes; default is dry-run")
    parser.add_argument("--dry-run", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--assume-frozen", action="store_true", help="confirm source writes are frozen for this pass")
    parser.add_argument(
        "--allow-live-copy",
        action="store_true",
        help="allow an initial live copy pass; final correctness still requires a later frozen/verified pass",
    )
    parser.add_argument("--replace", action="store_true", help="overwrite existing destination keys")
    parser.add_argument("--cap-policy", choices=("fail", "skip"), default="fail")
    parser.add_argument("--strict-scan", action="store_true", help="fail when scanned keys are not redis-roaring values")
    parser.add_argument("--keep-going", action="store_true", help="continue after per-key migration errors")
    parser.add_argument("--scan-count", type=int, default=1000)
    parser.add_argument("--page-size", type=int, default=10000)
    parser.add_argument("--pipeline-size", type=int, default=1000)
    parser.add_argument("--sample-count", type=int, default=64)
    parser.add_argument("--full-diff-limit", type=int, default=10000)
    parser.add_argument("--socket-timeout", type=float, default=30.0)
    parser.add_argument("--allow-cluster", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.apply and args.dry_run:
        raise MigrateError("--apply and --dry-run are mutually exclusive")
    args.dry_run = not args.apply
    if args.page_size <= 0:
        raise MigrateError("--page-size must be positive")
    if args.pipeline_size <= 0:
        raise MigrateError("--pipeline-size must be positive")
    if args.sample_count < 0:
        raise MigrateError("--sample-count must not be negative")
    if args.full_diff_limit < 0:
        raise MigrateError("--full-diff-limit must not be negative")
    if args.all_dbs and args.db:
        raise MigrateError("--all-dbs cannot be combined with --db")
    if args.db is None:
        args.db = [0]
    return args


def main(argv: list[str]) -> int:
    try:
        args = parse_args(argv)
        return BitmapMigrator(args).run()
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    except MigrateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
