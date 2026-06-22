#!/usr/bin/env python3
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional


QPS_RE = re.compile(r"([0-9]+(?:\.[0-9]+)?)\s+requests per second")


DATASET_KEYS = {
    "dense_legacy": "bench:bitmap:dense:legacy",
    "dense_native": "bench:bitmap:dense:native",
    "sparse_legacy": "bench:bitmap:sparse:legacy",
    "sparse_native": "bench:bitmap:sparse:native",
    "clustered_legacy": "bench:bitmap:clustered:legacy",
    "clustered_native": "bench:bitmap:clustered:native",
    "mixed_legacy_a": "bench:bitmap:mixed:legacy:a",
    "mixed_native_b": "bench:bitmap:mixed:native:b",
    "mixed_legacy_c": "bench:bitmap:mixed:legacy:c",
    "mixed_native_d": "bench:bitmap:mixed:native:d",
}


class BenchError(RuntimeError):
    pass


class RespConnection:
    def __init__(self, host: str, port: int, db: int, timeout: float):
        self.host = host
        self.port = port
        self.db = db
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self.fp = None

    def __enter__(self) -> "RespConnection":
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        self.fp = self.sock.makefile("rb")
        if self.db:
            self.command(["SELECT", str(self.db)])
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self.fp is not None:
            self.fp.close()
        if self.sock is not None:
            self.sock.close()

    def command(self, parts: list[Any]) -> Any:
        if self.sock is None:
            raise BenchError("RESP connection is not open")
        self.sock.sendall(encode_resp(parts))
        return self.read_response()

    def send_pipeline(self, commands: list[list[Any]]) -> list[Any]:
        if self.sock is None:
            raise BenchError("RESP connection is not open")
        self.sock.sendall(b"".join(encode_resp(cmd) for cmd in commands))
        return [self.read_response() for _ in commands]

    def read_response(self) -> Any:
        if self.fp is None:
            raise BenchError("RESP connection is not open")
        prefix = self.fp.read(1)
        if not prefix:
            raise BenchError("redis closed the connection")
        if prefix == b"+":
            return self._read_line().decode("utf-8", "replace")
        if prefix == b"-":
            raise BenchError(self._read_line().decode("utf-8", "replace"))
        if prefix == b":":
            return int(self._read_line())
        if prefix == b"$":
            length = int(self._read_line())
            if length == -1:
                return None
            data = self.fp.read(length)
            crlf = self.fp.read(2)
            if crlf != b"\r\n":
                raise BenchError("invalid bulk string terminator")
            return data
        if prefix == b"*":
            length = int(self._read_line())
            if length == -1:
                return None
            return [self.read_response() for _ in range(length)]
        raise BenchError(f"unknown RESP prefix: {prefix!r}")

    def _read_line(self) -> bytes:
        if self.fp is None:
            raise BenchError("RESP connection is not open")
        line = self.fp.readline()
        if not line.endswith(b"\r\n"):
            raise BenchError("invalid RESP line")
        return line[:-2]


class RedisClient:
    def __init__(self, host: str, port: int, db: int, timeout: float):
        self.host = host
        self.port = port
        self.db = db
        self.timeout = timeout

    def connection(self) -> RespConnection:
        return RespConnection(self.host, self.port, self.db, self.timeout)

    def execute(self, command: list[Any]) -> Any:
        with self.connection() as conn:
            return conn.command(command)

    def pipeline(self, commands: Iterable[list[Any]], chunk_size: int = 1000) -> None:
        chunk: list[list[Any]] = []
        with self.connection() as conn:
            for command in commands:
                chunk.append(command)
                if len(chunk) >= chunk_size:
                    conn.send_pipeline(chunk)
                    chunk.clear()
            if chunk:
                conn.send_pipeline(chunk)


def encode_resp(parts: list[Any]) -> bytes:
    out = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        data = part if isinstance(part, bytes) else str(part).encode("utf-8")
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


def parse_info(raw: Any) -> dict[str, str]:
    text = decode_text(raw)
    out: dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith("#") or ":" not in line:
            continue
        key, value = line.split(":", 1)
        out[key] = value
    return out


def dense_payload(length: int) -> bytes:
    pattern = b"\xff\xfe\xff\x7f"
    repeat, extra = divmod(length, len(pattern))
    return pattern * repeat + pattern[:extra]


def mixed_payload(length: int, salt: int) -> bytes:
    return bytes(((i * 37 + salt) & 0xFF) for i in range(length))


@dataclass
class Workload:
    name: str
    description: str
    command: list[str]
    requests: int
    clients: int
    pipeline: int
    rand_range: int = 0
    warmup_requests: int = 2000
    setup: Optional[str] = None
    sample_key: Optional[str] = None
    one_shot: bool = False


@dataclass
class DatasetSummary:
    name: str
    key: str
    redis_type: str
    encoding: str
    bitcount: int
    memory_usage_bytes: Optional[int]
    dump_payload_bytes: Optional[int]


@dataclass
class Result:
    name: str
    category: str
    description: str
    elapsed_ms: float
    memory_usage_bytes: Optional[int]
    used_memory_before: Optional[int]
    used_memory_after: Optional[int]
    used_memory_peak: Optional[int]
    payload_size_bytes: Optional[int]
    qps: Optional[float] = None
    requests: Optional[int] = None
    clients: Optional[int] = None
    pipeline: Optional[int] = None
    rand_range: Optional[int] = None
    command: Optional[list[str]] = None
    extra: dict[str, Any] = field(default_factory=dict)


class RedisBitmapBench:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.base_dir = Path(__file__).resolve().parent
        self.repo_root = self.base_dir.parent
        src_dir = Path(args.src_dir) if args.src_dir else self.repo_root / "src"
        self.redis_server = self.resolve_binary(src_dir, "redis-server")
        self.redis_cli = self.resolve_binary(src_dir, "redis-cli")
        self.redis_benchmark = self.resolve_binary(src_dir, "redis-benchmark")
        self.host = args.host
        self.port = args.port
        self.db = args.db
        self.client = RedisClient(self.host, self.port, self.db, args.socket_timeout)
        self.server_proc: Optional[subprocess.Popen[Any]] = None
        self.server_dir: Optional[tempfile.TemporaryDirectory[str]] = None
        self.data_dir: Optional[Path] = None
        self.server_log = None
        self.server_log_path: Optional[Path] = None
        self.datasets: list[DatasetSummary] = []
        self.results: list[Result] = []
        self.persistence_results: list[Result] = []

    @staticmethod
    def resolve_binary(src_dir: Path, name: str) -> str:
        candidates = [src_dir / name]
        if os.name == "nt":
            candidates.append(src_dir / f"{name}.exe")
        for candidate in candidates:
            if candidate.exists():
                return str(candidate)
        raise BenchError(f"missing binary: {candidates[0]}")

    def run(self) -> int:
        try:
            if self.args.start_server:
                self.start_server()
            self.prepare_data()
            self.datasets = self.dataset_summary()
            self.print_dataset_summary()

            for workload in self.selected_workloads():
                result = self.run_workload(workload)
                self.results.append(result)
                payload = "-" if result.payload_size_bytes is None else str(result.payload_size_bytes)
                metric = f"{result.qps:.2f} req/s" if result.qps is not None else "one-shot"
                print(
                    f"{result.name:32s} {metric:>18s} "
                    f"{result.elapsed_ms:9.2f} ms payload={payload}"
                )

            if self.args.skip_persistence:
                print("persistence benchmarks skipped by --skip-persistence", file=sys.stderr)
            elif not self.args.start_server:
                print("persistence benchmarks skipped: --start-server is required", file=sys.stderr)
            else:
                self.run_persistence_suite()

            self.print_summary()
            if self.args.json_out:
                self.write_json()
            return 0
        finally:
            if self.args.start_server and not self.args.keep_server:
                self.stop_server(cleanup=True)

    def start_server(self) -> None:
        if self.data_dir is None:
            if self.args.keep_server:
                self.data_dir = Path(tempfile.mkdtemp(prefix="bitmap-bench-"))
                print(f"keeping redis-server data dir: {self.data_dir}", file=sys.stderr)
            else:
                self.server_dir = tempfile.TemporaryDirectory(prefix="bitmap-bench-")
                self.data_dir = Path(self.server_dir.name)
            self.server_log_path = self.data_dir / "redis-server.log"
        self.launch_server()

    def launch_server(self) -> None:
        if self.data_dir is None:
            raise BenchError("server data dir is not initialized")
        self.server_log = open(self.server_log_path, "ab") if self.server_log_path else subprocess.DEVNULL
        cmd = [
            self.redis_server,
            "--bind", self.host,
            "--port", str(self.port),
            "--save", "",
            "--appendonly", "no",
            "--appenddirname", "appendonlydir",
            "--aof-use-rdb-preamble", "no",
            "--auto-aof-rewrite-percentage", "0",
            "--dir", str(self.data_dir),
            "--dbfilename", "dump.rdb",
            "--loglevel", "warning",
            "--daemonize", "no",
        ]
        self.server_proc = subprocess.Popen(
            cmd,
            stdout=self.server_log,
            stderr=subprocess.STDOUT,
        )
        self.wait_for_ping(timeout=10.0)

    def stop_server(self, cleanup: bool) -> None:
        if self.server_proc is not None and self.server_proc.poll() is None:
            try:
                self.client.execute(["SHUTDOWN", "NOSAVE"])
            except Exception:
                pass
            try:
                self.server_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server_proc.send_signal(signal.SIGTERM)
                try:
                    self.server_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.server_proc.kill()
                    self.server_proc.wait(timeout=5)
        if self.server_log not in (None, subprocess.DEVNULL):
            self.server_log.close()
        self.server_proc = None
        self.server_log = None
        if cleanup and self.server_dir is not None:
            self.server_dir.cleanup()
            self.server_dir = None
            self.data_dir = None
            self.server_log_path = None

    def restart_server_for_rdb_load(self) -> float:
        self.stop_server(cleanup=False)
        started = time.perf_counter()
        self.launch_server()
        return elapsed_ms(started)

    def wait_for_ping(self, timeout: float) -> None:
        deadline = time.time() + timeout
        last_error: Any = None
        while time.time() < deadline:
            if self.server_proc is not None and self.server_proc.poll() is not None:
                raise BenchError(
                    "server exited before becoming ready:\n"
                    f"{self.read_server_log().strip()}"
                )
            try:
                if self.client.execute(["PING"]) == "PONG" and self.ping_is_from_child():
                    return
            except Exception as exc:
                last_error = exc
            time.sleep(0.05)
        raise BenchError(
            f"server did not start on {self.host}:{self.port}: {last_error}\n"
            f"{self.read_server_log().strip()}"
        )

    def ping_is_from_child(self) -> bool:
        if self.server_proc is None:
            return False
        info = parse_info(self.client.execute(["INFO", "SERVER"]))
        process_id = to_int(info.get("process_id"))
        if process_id == self.server_proc.pid:
            return True
        if process_id is None:
            raise BenchError("server answered PING but INFO SERVER did not report process_id")
        raise BenchError(
            f"server answered PING from pid {process_id}, "
            f"expected benchmark child pid {self.server_proc.pid}"
        )

    def read_server_log(self) -> str:
        if not self.server_log_path or not self.server_log_path.exists():
            return ""
        return self.server_log_path.read_text(encoding="utf-8", errors="replace")

    def prepare_data(self) -> None:
        print("preparing bitmap datasets...", file=sys.stderr)
        self.client.execute(["FLUSHDB"])
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "no"])

        dense = dense_payload(self.args.dense_bytes)
        self.client.execute(["SET", DATASET_KEYS["dense_legacy"], dense])
        self.client.execute(["SET", DATASET_KEYS["dense_native"], dense])
        self.client.execute(["BITMAP", "CONVERT", DATASET_KEYS["dense_native"], "NATIVE"])

        sparse_bits = self.sparse_bits()
        clustered_bits = self.clustered_bits()
        self.seed_setbit_bitmap(DATASET_KEYS["sparse_legacy"], sparse_bits, native=False)
        self.seed_setbit_bitmap(DATASET_KEYS["sparse_native"], sparse_bits, native=True)
        self.seed_setbit_bitmap(DATASET_KEYS["clustered_legacy"], clustered_bits, native=False)
        self.seed_setbit_bitmap(DATASET_KEYS["clustered_native"], clustered_bits, native=True)

        mixed_len = min(self.args.dense_bytes, self.args.mixed_bytes)
        self.client.execute(["SET", DATASET_KEYS["mixed_legacy_a"], mixed_payload(mixed_len, 11)])
        self.client.execute(["SET", DATASET_KEYS["mixed_native_b"], mixed_payload(mixed_len, 37)])
        self.client.execute(["BITMAP", "CONVERT", DATASET_KEYS["mixed_native_b"], "NATIVE"])
        self.client.execute(["SET", DATASET_KEYS["mixed_legacy_c"], mixed_payload(mixed_len, 71)])
        self.client.execute(["SET", DATASET_KEYS["mixed_native_d"], mixed_payload(mixed_len, 109)])
        self.client.execute(["BITMAP", "CONVERT", DATASET_KEYS["mixed_native_d"], "NATIVE"])
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "no"])

    def sparse_bits(self) -> list[int]:
        count = max(1, self.args.sparse_count)
        stride = max(1, self.args.sparse_space // count)
        return [min(self.args.sparse_space - 1, i * stride) for i in range(count)]

    def clustered_bits(self) -> list[int]:
        bits: list[int] = []
        for cluster in range(self.args.cluster_count):
            base = cluster * self.args.cluster_gap
            for bit in range(base, base + self.args.cluster_span):
                bits.append(bit)
        return bits

    def seed_setbit_bitmap(self, key: str, bits: list[int], native: bool) -> None:
        self.client.execute(["DEL", key])
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "yes" if native else "no"])
        if not bits:
            self.client.execute(["SET", key, b""])
            if native:
                self.client.execute(["BITMAP", "CONVERT", key, "NATIVE"])
            return
        self.client.pipeline((["SETBIT", key, bit, "1"] for bit in bits))
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "no"])

    def dataset_summary(self) -> list[DatasetSummary]:
        summaries = []
        for name, key in DATASET_KEYS.items():
            summaries.append(DatasetSummary(
                name=name,
                key=key,
                redis_type=decode_text(self.client.execute(["TYPE", key])),
                encoding=decode_text(self.client.execute(["OBJECT", "ENCODING", key])),
                bitcount=int(self.client.execute(["BITCOUNT", key])),
                memory_usage_bytes=self.memory_usage(key),
                dump_payload_bytes=self.dump_payload_size(key),
            ))
        return summaries

    def selected_workloads(self) -> list[Workload]:
        workloads = self.workloads()
        if not self.args.only:
            return workloads
        wanted = {name.strip() for name in self.args.only.split(",") if name.strip()}
        unknown = wanted - {w.name for w in workloads}
        if unknown:
            raise BenchError(f"unknown workload(s): {', '.join(sorted(unknown))}")
        return [w for w in workloads if w.name in wanted]

    def workloads(self) -> list[Workload]:
        return [
            Workload(
                "setbit_native_create_sparse",
                "One-shot SETBIT latency creating a missing sparse key as a native bitmap",
                ["SETBIT", "bench:bitmap:setbit:create", str(self.args.sparse_space - 1), "1"],
                1, 1, 1,
                warmup_requests=0, setup="setup_setbit_native_create",
                sample_key="bench:bitmap:setbit:create",
                one_shot=True,
            ),
            Workload(
                "setbit_convert_string_dense",
                "One-shot SETBIT latency converting an existing legacy string bitmap to native before writing",
                ["SETBIT", "bench:bitmap:setbit:convert", str(self.args.convert_bytes * 8 - 1), "1"],
                1, 1, 1,
                warmup_requests=0, setup="setup_setbit_convert_string",
                sample_key="bench:bitmap:setbit:convert",
                one_shot=True,
            ),
            Workload(
                "bitcount_dense_legacy",
                "BITCOUNT over a dense legacy string bitmap",
                ["BITCOUNT", DATASET_KEYS["dense_legacy"]],
                50_000, 32, 8, sample_key=DATASET_KEYS["dense_legacy"],
            ),
            Workload(
                "bitcount_dense_native",
                "BITCOUNT over a dense native bitmap",
                ["BITCOUNT", DATASET_KEYS["dense_native"]],
                50_000, 32, 8, sample_key=DATASET_KEYS["dense_native"],
            ),
            Workload(
                "bitcount_sparse_native",
                "BITCOUNT over a sparse native bitmap",
                ["BITCOUNT", DATASET_KEYS["sparse_native"]],
                50_000, 32, 8, sample_key=DATASET_KEYS["sparse_native"],
            ),
            Workload(
                "bitpos_clustered_native",
                "BITPOS over clustered native runs",
                ["BITPOS", DATASET_KEYS["clustered_native"], "1"],
                40_000, 24, 8, sample_key=DATASET_KEYS["clustered_native"],
            ),
            Workload(
                "bitfield_ro_native_hot",
                "BITFIELD_RO GET over a clustered native offset",
                ["BITFIELD_RO", DATASET_KEYS["clustered_native"], "GET", "u8", "0"],
                80_000, 32, 16,
                sample_key=DATASET_KEYS["clustered_native"],
            ),
            Workload(
                "bitfield_set_native_hot",
                "BITFIELD SET writes into a native bitmap",
                ["BITFIELD", "bench:bitmap:bitfield:write", "SET", "u1", str(self.args.sparse_space - 1), "1"],
                50_000, 24, 8,
                setup="setup_bitfield_native_write",
                sample_key="bench:bitmap:bitfield:write",
            ),
            Workload(
                "bitop_or_mixed",
                "BITOP OR with mixed legacy string and native bitmap sources",
                ["BITOP", "OR", "bench:bitmap:bitop:or:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:or:dest",
            ),
            Workload(
                "bitop_xor_mixed",
                "BITOP XOR with mixed legacy string and native bitmap sources",
                ["BITOP", "XOR", "bench:bitmap:bitop:xor:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:xor:dest",
            ),
            Workload(
                "bitop_not_native",
                "BITOP NOT from a native bitmap source",
                ["BITOP", "NOT", "bench:bitmap:bitop:not:dest", DATASET_KEYS["clustered_native"]],
                10_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:not:dest",
            ),
            Workload(
                "bitop_diff1_mixed",
                "BITOP DIFF1 over mixed legacy string and native bitmap sources",
                ["BITOP", "DIFF1", "bench:bitmap:bitop:diff1:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:diff1:dest",
            ),
            Workload(
                "bitop_one_mixed",
                "BITOP ONE over mixed legacy string and native bitmap sources",
                ["BITOP", "ONE", "bench:bitmap:bitop:one:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:one:dest",
            ),
        ]

    def setup_setbit_native_create(self) -> None:
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "yes"])
        self.client.execute(["DEL", "bench:bitmap:setbit:create"])

    def setup_setbit_convert_string(self) -> None:
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "no"])
        self.client.execute(["DEL", "bench:bitmap:setbit:convert"])
        self.client.execute(["SET", "bench:bitmap:setbit:convert", dense_payload(self.args.convert_bytes)])
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "yes"])

    def setup_bitfield_native_write(self) -> None:
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "no"])
        self.client.execute(["DEL", "bench:bitmap:bitfield:write"])
        self.client.execute(["SET", "bench:bitmap:bitfield:write", b""])
        self.client.execute(["BITMAP", "CONVERT", "bench:bitmap:bitfield:write", "NATIVE"])

    def setup_bitop_mixed(self) -> None:
        self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "no"])
        self.client.execute(["DEL",
                             "bench:bitmap:bitop:or:dest",
                             "bench:bitmap:bitop:xor:dest",
                             "bench:bitmap:bitop:not:dest",
                             "bench:bitmap:bitop:diff1:dest",
                             "bench:bitmap:bitop:one:dest"])

    def run_workload(self, workload: Workload) -> Result:
        if workload.setup:
            getattr(self, workload.setup)()
        if workload.one_shot:
            return self.run_one_shot_workload(workload)
        if self.args.warmup and workload.warmup_requests > 0:
            self.invoke_benchmark(workload, self.scale_requests(workload.warmup_requests))

        before = self.memory_snapshot()
        start = time.perf_counter()
        raw = self.invoke_benchmark(workload, self.scale_requests(workload.requests))
        elapsed = elapsed_ms(start)
        after = self.memory_snapshot()
        qps = self.parse_qps(raw)
        key = workload.sample_key
        return Result(
            name=workload.name,
            category="command",
            description=workload.description,
            elapsed_ms=elapsed,
            memory_usage_bytes=self.memory_usage(key) if key else None,
            used_memory_before=before.get("used_memory"),
            used_memory_after=after.get("used_memory"),
            used_memory_peak=after.get("used_memory_peak"),
            payload_size_bytes=self.dump_payload_size(key) if key else None,
            qps=qps,
            requests=self.scale_requests(workload.requests),
            clients=workload.clients,
            pipeline=workload.pipeline,
            rand_range=workload.rand_range,
            command=workload.command,
            extra={"raw_output": raw.strip()},
        )

    def run_one_shot_workload(self, workload: Workload) -> Result:
        before = self.memory_snapshot()
        with self.client.connection() as conn:
            start = time.perf_counter()
            response = conn.command(workload.command)
            elapsed = elapsed_ms(start)
        after = self.memory_snapshot()
        key = workload.sample_key
        return Result(
            name=workload.name,
            category="command",
            description=workload.description,
            elapsed_ms=elapsed,
            memory_usage_bytes=self.memory_usage(key) if key else None,
            used_memory_before=before.get("used_memory"),
            used_memory_after=after.get("used_memory"),
            used_memory_peak=after.get("used_memory_peak"),
            payload_size_bytes=self.dump_payload_size(key) if key else None,
            requests=1,
            clients=1,
            pipeline=1,
            rand_range=workload.rand_range,
            command=workload.command,
            extra={
                "measurement": "one-shot latency",
                "response": decode_text(response),
            },
        )

    def invoke_benchmark(self, workload: Workload, requests: int) -> str:
        cmd = [
            self.redis_benchmark,
            "-h", self.host,
            "-p", str(self.port),
            "--dbnum", str(self.db),
            "-n", str(requests),
            "-c", str(workload.clients),
            "-P", str(workload.pipeline),
            "--seed", str(self.args.seed),
            "-q",
        ]
        if workload.rand_range:
            cmd.extend(["-r", str(workload.rand_range)])
        cmd.extend(workload.command)
        return subprocess.check_output(
            cmd,
            text=True,
            stderr=subprocess.STDOUT,
            timeout=self.args.benchmark_timeout,
        )

    @staticmethod
    def parse_qps(raw: str) -> float:
        match = QPS_RE.search(raw)
        if not match:
            raise BenchError(f"could not parse qps from redis-benchmark output:\n{raw}")
        return float(match.group(1))

    def scale_requests(self, requests: int) -> int:
        return max(1, int(requests * self.args.request_scale))

    def run_persistence_suite(self) -> None:
        print("\nrunning persistence benchmarks...", file=sys.stderr)
        self.prepare_data()
        for name in ("dense_legacy", "dense_native", "sparse_native", "clustered_native",
                     "mixed_legacy_a", "mixed_native_b"):
            self.persistence_results.append(self.run_dump_restore(name, DATASET_KEYS[name]))

        self.prepare_data()
        self.persistence_results.append(self.run_rdb_save_load())
        self.persistence_results.append(self.run_aof_rewrite())

    def run_dump_restore(self, name: str, key: str) -> Result:
        before = self.memory_snapshot()
        dest = f"{key}:restored"
        with self.client.connection() as conn:
            started = time.perf_counter()
            payload = conn.command(["DUMP", key])
            dump_elapsed = elapsed_ms(started)
            if not isinstance(payload, bytes):
                raise BenchError(f"DUMP returned no payload for {key}")

            conn.command(["DEL", dest])
            restore_started = time.perf_counter()
            conn.command(["RESTORE", dest, "0", payload])
            restore_elapsed = elapsed_ms(restore_started)
        after = self.memory_snapshot()
        return Result(
            name=f"dump_restore_{name}",
            category="persistence",
            description=f"DUMP and RESTORE round trip for {key}",
            elapsed_ms=dump_elapsed + restore_elapsed,
            memory_usage_bytes=self.memory_usage(dest),
            used_memory_before=before.get("used_memory"),
            used_memory_after=after.get("used_memory"),
            used_memory_peak=after.get("used_memory_peak"),
            payload_size_bytes=len(payload),
            command=["DUMP", key, "RESTORE", dest],
            extra={
                "dump_ms": dump_elapsed,
                "restore_ms": restore_elapsed,
                "restored_type": decode_text(self.client.execute(["TYPE", dest])),
                "restored_encoding": decode_text(self.client.execute(["OBJECT", "ENCODING", dest])),
            },
        )

    def run_rdb_save_load(self) -> Result:
        before = self.memory_snapshot()
        started = time.perf_counter()
        self.client.execute(["SAVE"])
        save_elapsed = elapsed_ms(started)
        rdb_path = self.require_data_dir() / "dump.rdb"
        payload_size = rdb_path.stat().st_size if rdb_path.exists() else None
        load_elapsed = self.restart_server_for_rdb_load()
        after = self.memory_snapshot()
        return Result(
            name="rdb_save_load",
            category="persistence",
            description="Synchronous RDB SAVE followed by measured server restart/load",
            elapsed_ms=save_elapsed + load_elapsed,
            memory_usage_bytes=None,
            used_memory_before=before.get("used_memory"),
            used_memory_after=after.get("used_memory"),
            used_memory_peak=after.get("used_memory_peak"),
            payload_size_bytes=payload_size,
            command=["SAVE", "restart redis-server"],
            extra={
                "save_ms": save_elapsed,
                "load_ms": load_elapsed,
                "dbsize_after_load": int(self.client.execute(["DBSIZE"])),
            },
        )

    def run_aof_rewrite(self) -> Result:
        before = self.memory_snapshot()
        self.client.execute(["CONFIG", "SET", "appendonly", "yes"])
        self.wait_for_aof_rewrite()
        started = time.perf_counter()
        self.client.execute(["BGREWRITEAOF"])
        self.wait_for_aof_rewrite()
        rewrite_elapsed = elapsed_ms(started)
        persistence = parse_info(self.client.execute(["INFO", "PERSISTENCE"]))
        aof_current_size = to_int(persistence.get("aof_current_size"))
        after = self.memory_snapshot()
        return Result(
            name="aof_rewrite",
            category="persistence",
            description="Command-format AOF rewrite with native bitmap RESTORE payloads",
            elapsed_ms=rewrite_elapsed,
            memory_usage_bytes=None,
            used_memory_before=before.get("used_memory"),
            used_memory_after=after.get("used_memory"),
            used_memory_peak=after.get("used_memory_peak"),
            payload_size_bytes=aof_current_size,
            command=["CONFIG SET appendonly yes", "BGREWRITEAOF"],
            extra={
                "aof_last_bgrewrite_status": persistence.get("aof_last_bgrewrite_status"),
                "aof_current_size": aof_current_size,
            },
        )

    def wait_for_aof_rewrite(self) -> None:
        deadline = time.time() + self.args.persistence_timeout
        last = {}
        while time.time() < deadline:
            last = parse_info(self.client.execute(["INFO", "PERSISTENCE"]))
            if last.get("aof_rewrite_in_progress") == "0":
                status = last.get("aof_last_bgrewrite_status", "ok")
                if status == "ok":
                    return
                raise BenchError(f"AOF rewrite failed: {status}")
            time.sleep(0.05)
        raise BenchError(f"timed out waiting for AOF rewrite: {last}")

    def require_data_dir(self) -> Path:
        if self.data_dir is None:
            raise BenchError("server data dir is not available")
        return self.data_dir

    def memory_snapshot(self) -> dict[str, Optional[int]]:
        info = parse_info(self.client.execute(["INFO", "MEMORY"]))
        return {
            "used_memory": to_int(info.get("used_memory")),
            "used_memory_peak": to_int(info.get("used_memory_peak")),
        }

    def memory_usage(self, key: Optional[str]) -> Optional[int]:
        if not key:
            return None
        value = self.client.execute(["MEMORY", "USAGE", key])
        return value if isinstance(value, int) else None

    def dump_payload_size(self, key: Optional[str]) -> Optional[int]:
        if not key:
            return None
        payload = self.client.execute(["DUMP", key])
        return len(payload) if isinstance(payload, bytes) else None

    def print_dataset_summary(self) -> None:
        print("dataset:")
        print("| name | type | encoding | bitcount | memory bytes | dump bytes |")
        print("|---|---|---|---:|---:|---:|")
        for item in self.datasets:
            print(
                f"| {item.name} | {item.redis_type} | {item.encoding} | "
                f"{item.bitcount} | {fmt_optional(item.memory_usage_bytes)} | "
                f"{fmt_optional(item.dump_payload_bytes)} |"
            )

    def print_summary(self) -> None:
        print("\ncommand summary:")
        print("| workload | qps | elapsed/latency ms | memory bytes | peak bytes | payload bytes |")
        print("|---|---:|---:|---:|---:|---:|")
        for result in self.results:
            qps = "-" if result.qps is None else f"{result.qps:.2f}"
            print(
                f"| {result.name} | {qps} | {result.elapsed_ms:.2f} | "
                f"{fmt_optional(result.memory_usage_bytes)} | "
                f"{fmt_optional(result.used_memory_peak)} | "
                f"{fmt_optional(result.payload_size_bytes)} |"
            )

        if self.persistence_results:
            print("\npersistence summary:")
            print("| operation | elapsed ms | memory bytes | peak bytes | payload bytes | notes |")
            print("|---|---:|---:|---:|---:|---|")
            for result in self.persistence_results:
                notes = ", ".join(f"{k}={v}" for k, v in result.extra.items() if k.endswith("_ms") or k.endswith("_status"))
                print(
                    f"| {result.name} | {result.elapsed_ms:.2f} | "
                    f"{fmt_optional(result.memory_usage_bytes)} | "
                    f"{fmt_optional(result.used_memory_peak)} | "
                    f"{fmt_optional(result.payload_size_bytes)} | {notes} |"
                )

    def write_json(self) -> None:
        payload = {
            "host": self.host,
            "port": self.port,
            "db": self.db,
            "parameters": {
                "dense_bytes": self.args.dense_bytes,
                "sparse_space": self.args.sparse_space,
                "sparse_count": self.args.sparse_count,
                "cluster_count": self.args.cluster_count,
                "cluster_span": self.args.cluster_span,
                "cluster_gap": self.args.cluster_gap,
                "request_scale": self.args.request_scale,
            },
            "datasets": [asdict(d) for d in self.datasets],
            "results": [asdict(r) for r in self.results],
            "persistence": [asdict(r) for r in self.persistence_results],
        }
        with open(self.args.json_out, "w", encoding="utf-8") as fp:
            json.dump(payload, fp, indent=2)
        print(f"json written to {self.args.json_out}")


def elapsed_ms(started: float) -> float:
    return (time.perf_counter() - started) * 1000.0


def to_int(value: Optional[str]) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def fmt_optional(value: Optional[int]) -> str:
    return "-" if value is None else str(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Standalone Redis native bitmap benchmark harness. It starts an "
            "ephemeral server by default, requires no redis.conf or default "
            "configuration changes, loads deterministic sparse/dense/clustered/"
            "mixed datasets, and measures command, DUMP/RESTORE, RDB, and AOF "
            "rewrite behavior."
        )
    )
    parser.add_argument("--src-dir", help="Path to src containing redis-server, redis-cli, and redis-benchmark")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6396)
    parser.add_argument("--db", type=int, default=9)
    parser.add_argument("--start-server", action="store_true", default=True,
                        help="Start an ephemeral redis-server on --port (default: enabled)")
    parser.add_argument("--no-start-server", dest="start_server", action="store_false",
                        help="Use an already running server; persistence phases are skipped")
    parser.add_argument("--keep-server", action="store_true",
                        help="Do not stop the ephemeral server after the run")
    parser.add_argument("--only", help="Comma-separated command workload names to run")
    parser.add_argument("--skip-persistence", action="store_true",
                        help="Skip DUMP/RESTORE, RDB save/load, and AOF rewrite phases")
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--request-scale", type=float, default=1.0,
                        help="Scale factor applied to command workload request counts")
    parser.add_argument("--warmup", action="store_true", default=True,
                        help="Run workload warmups where defined (default: enabled)")
    parser.add_argument("--no-warmup", dest="warmup", action="store_false")
    parser.add_argument("--json-out", help="Optional path for machine-readable results")
    parser.add_argument("--dense-bytes", type=int, default=131_072,
                        help="Logical byte length for dense bitmap datasets")
    parser.add_argument("--mixed-bytes", type=int, default=65_536,
                        help="Logical byte length for mixed BITOP source datasets")
    parser.add_argument("--convert-bytes", type=int, default=65_536,
                        help="Legacy string byte length for SETBIT conversion workload")
    parser.add_argument("--sparse-space", type=int, default=16_777_216,
                        help="Logical bit space used by sparse benchmarks")
    parser.add_argument("--sparse-count", type=int, default=4096,
                        help="Number of set bits in sparse datasets")
    parser.add_argument("--cluster-count", type=int, default=8)
    parser.add_argument("--cluster-span", type=int, default=512,
                        help="Set bits per clustered run")
    parser.add_argument("--cluster-gap", type=int, default=262_144,
                        help="Bit distance between clustered runs")
    parser.add_argument("--socket-timeout", type=float, default=30.0)
    parser.add_argument("--benchmark-timeout", type=float, default=120.0)
    parser.add_argument("--persistence-timeout", type=float, default=120.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        bench = RedisBitmapBench(args)
        return bench.run()
    except BenchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        print(exc.output, file=sys.stderr)
        return exc.returncode or 1
    except subprocess.TimeoutExpired as exc:
        print(f"error: command timed out: {' '.join(exc.cmd)}", file=sys.stderr)
        if exc.output:
            print(exc.output, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
