#!/usr/bin/env python3
import argparse
import csv
import json
import math
import os
import platform
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import asdict, dataclass, field, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Optional
from urllib.request import urlretrieve
from zipfile import ZipFile


QPS_RE = re.compile(r"([0-9]+(?:\.[0-9]+)?)\s+requests per second")
INTEGER_RE = re.compile(r"\d+")
REALDATA_BASE_URL = "https://raw.githubusercontent.com/RoaringBitmap/real-roaring-datasets/master"
REALDATA_ARCHIVES = (
    "census-income",
    "census-income_srt",
    "census1881",
    "census1881_srt",
    "uscensus2000",
    "weather_sept_85",
    "weather_sept_85_srt",
    "wikileaks-noquotes",
    "wikileaks-noquotes_srt",
)
CSV_FIELDS = [
    "mode",
    "target_groups",
    "category",
    "story",
    "dataset",
    "workload",
    "metric",
    "value",
    "elapsed_ms",
    "time_per_op_us",
    "time_per_op_mean_us",
    "time_per_op_median_us",
    "time_per_op_min_us",
    "time_per_op_max_us",
    "time_per_op_stdev_us",
    "time_per_op_cv_percent",
    "qps",
    "memory_bytes",
    "peak_bytes",
    "payload_bytes",
    "stall_ms",
    "notes",
]


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

MODULE_RESULT_LABEL = "redis_roaring_module"
COMPARE_LABELS = (
    "redis_before",
    "redis_pr_native",
    MODULE_RESULT_LABEL,
)
PROFILE_GROUPS = {
    "full": ("small-sets", "bitsets", "mixed-bitop"),
    "small-sets": ("small-sets",),
    "bitsets": ("bitsets",),
    "mixed-bitop": ("mixed-bitop",),
}
SMOKE_WORKLOADS = {
    "setbit_native_create_sparse",
    "getbit_sparse_native_hit",
    "bitcount_dense_native",
    "bitop_and_mixed",
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


def iter_payload_set_bits(payload: bytes) -> Iterable[int]:
    for byte_index, value in enumerate(payload):
        base = byte_index * 8
        for bit_index in range(8):
            if value & (0x80 >> bit_index):
                yield base + bit_index


def chunked_ints(values: Iterable[int], chunk_size: int) -> Iterable[list[int]]:
    chunk: list[int] = []
    for value in values:
        chunk.append(value)
        if len(chunk) >= chunk_size:
            yield chunk
            chunk = []
    if chunk:
        yield chunk


def dataset_metrics_from_bits(bits: Iterable[int], logical_bits: Optional[int] = None) -> dict[str, Any]:
    values = list(bits)
    max_offset = max(values) if values else None
    if logical_bits is None:
        logical_bits = (max_offset + 1) if max_offset is not None else 0
    logical_bytes = (logical_bits + 7) // 8 if logical_bits else 0
    density = (len(values) / logical_bits) if logical_bits else 0.0
    return {
        "max_offset": max_offset,
        "logical_bytes": logical_bytes,
        "density": density,
    }


def dataset_metrics_from_payload(payload: bytes) -> dict[str, Any]:
    return dataset_metrics_from_bits(iter_payload_set_bits(payload), len(payload) * 8)


def time_per_op_us(elapsed_ms_value: float, requests: Optional[int], qps: Optional[float]) -> Optional[float]:
    if qps is not None and qps > 0:
        return 1_000_000.0 / qps
    if requests and requests > 0:
        return elapsed_ms_value * 1000.0 / requests
    return None


def sample_stats(values: list[float]) -> dict[str, Optional[float]]:
    if not values:
        return {
            "mean": None,
            "median": None,
            "min": None,
            "max": None,
            "stdev": None,
            "cv_percent": None,
        }
    mean = statistics.mean(values)
    stdev = statistics.stdev(values) if len(values) > 1 else 0.0
    return {
        "mean": mean,
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "stdev": stdev,
        "cv_percent": (stdev / mean * 100.0) if mean else None,
    }


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
    story: Optional[str] = None
    dataset: Optional[str] = None
    target_groups: tuple[str, ...] = ("bitsets",)
    native_only: bool = False
    stall_probe: bool = False


@dataclass
class DatasetSummary:
    name: str
    key: str
    redis_type: str
    encoding: str
    bitcount: int
    memory_usage_bytes: Optional[int]
    dump_payload_bytes: Optional[int]
    max_offset: Optional[int] = None
    logical_bytes: Optional[int] = None
    density: Optional[float] = None


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
    time_per_op_us: Optional[float] = None
    extra: dict[str, Any] = field(default_factory=dict)


@dataclass
class RealDataSet:
    name: str
    bits: list[int]


@dataclass
class StallStats:
    max_ping_ms: float
    max_gap_ms: float
    samples: int
    errors: int


class PingCanary:
    def __init__(self, client: RedisClient, interval: float, enabled: bool):
        self.client = client
        self.interval = interval
        self.enabled = enabled
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self.max_ping_ms = 0.0
        self.max_gap_ms = 0.0
        self.samples = 0
        self.errors = 0

    def __enter__(self) -> "PingCanary":
        if self.enabled:
            self.thread = threading.Thread(target=self._run, daemon=True)
            self.thread.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self.thread is not None:
            self.stop_event.set()
            self.thread.join(timeout=max(1.0, self.interval * 4))

    def _run(self) -> None:
        next_ping = time.perf_counter()
        while not self.stop_event.is_set():
            now = time.perf_counter()
            if now < next_ping:
                self.stop_event.wait(next_ping - now)
                continue
            gap_ms = max(0.0, (now - next_ping) * 1000.0)
            self.max_gap_ms = max(self.max_gap_ms, gap_ms)
            started = time.perf_counter()
            try:
                self.client.execute(["PING"])
                self.max_ping_ms = max(self.max_ping_ms, elapsed_ms(started))
                self.samples += 1
            except Exception:
                self.errors += 1
            next_ping += self.interval

    def stats(self) -> Optional[StallStats]:
        if not self.enabled:
            return None
        return StallStats(
            max_ping_ms=self.max_ping_ms,
            max_gap_ms=self.max_gap_ms,
            samples=self.samples,
            errors=self.errors,
        )


class RedisBitmapBench:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.base_dir = Path(__file__).resolve().parent
        self.repo_root = self.base_dir.parent
        src_dir = Path(args.src_dir) if args.src_dir else self.repo_root / "src"
        self.redis_server = self.resolve_binary(src_dir, "redis-server")
        self.redis_cli = self.resolve_binary(src_dir, "redis-cli")
        self.redis_benchmark = self.resolve_binary(src_dir, "redis-benchmark")
        self.src_dir = src_dir
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
        self.dataset_keys: dict[str, str] = dict(DATASET_KEYS)
        self.dataset_metadata: dict[str, dict[str, Any]] = {}
        self.realdata: list[RealDataSet] = []
        self.results: list[Result] = []
        self.persistence_results: list[Result] = []
        self.environment: dict[str, Any] = {}

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
            self.environment = self.collect_environment()
            self.prepare_data()
            self.datasets = self.dataset_summary()
            self.print_dataset_summary()

            for workload in self.selected_workloads():
                result = self.run_workload(workload)
                self.results.append(result)
                payload = "-" if result.payload_size_bytes is None else str(result.payload_size_bytes)
                if result.time_per_op_us is not None:
                    metric = f"{result.time_per_op_us:.2f} us/op"
                elif result.qps is not None:
                    metric = f"{result.qps:.2f} req/s"
                else:
                    metric = "measured"
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
            if self.args.csv_out:
                self.write_csv()
            if self.args.markdown_out:
                self.write_markdown()
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
        if self.args.mode == "module":
            cmd.extend(["--loadmodule", self.require_module_path()])
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

    def collect_environment(self) -> dict[str, Any]:
        info = {}
        try:
            info = parse_info(self.client.execute(["INFO", "SERVER"]))
        except Exception:
            pass
        return {
            "captured_at": datetime.now(timezone.utc).isoformat(),
            "mode": self.args.mode,
            "mode_label": self.args.mode_label,
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "runner_name": os.environ.get("RUNNER_NAME", "local"),
            "runner_os": os.environ.get("RUNNER_OS", platform.system()),
            "runner_arch": os.environ.get("RUNNER_ARCH", platform.machine()),
            "github_repository": os.environ.get("GITHUB_REPOSITORY", ""),
            "github_run_id": os.environ.get("GITHUB_RUN_ID", ""),
            "github_server_url": os.environ.get("GITHUB_SERVER_URL", "https://github.com"),
            "github_sha": os.environ.get("GITHUB_SHA", ""),
            "source_sha": self.git_sha_for_src_dir(),
            "source_repo_url": self.git_remote_for_src_dir(),
            "module_path": str(Path(self.args.module_path).resolve()) if self.args.module_path else "",
            "module_sha": self.git_sha_for_module(),
            "module_repo_url": self.git_remote_for_module(),
            "module_command_prefix": self.args.module_command_prefix if self.args.mode == "module" else "",
            "redis_version": info.get("redis_version", ""),
            "redis_git_sha1": info.get("redis_git_sha1", ""),
            "redis_build_id": info.get("redis_build_id", ""),
            "redis_server_version": self.binary_version(self.redis_server),
            "redis_benchmark_version": self.binary_version(self.redis_benchmark),
        }

    def require_module_path(self) -> str:
        if not self.args.module_path:
            raise BenchError("--module-path is required when --mode module starts redis-server")
        path = Path(self.args.module_path)
        if not path.exists():
            raise BenchError(f"missing redis-roaring module library: {path}")
        return str(path)

    def git_sha_for_src_dir(self) -> str:
        repo = self.src_dir.parent if self.src_dir.name == "src" else self.src_dir
        try:
            return subprocess.check_output(
                ["git", "-C", str(repo), "rev-parse", "HEAD"],
                text=True,
                stderr=subprocess.DEVNULL,
                timeout=5,
            ).strip()
        except Exception:
            return ""

    def git_remote_for_src_dir(self) -> str:
        repo = self.src_dir.parent if self.src_dir.name == "src" else self.src_dir
        return git_remote_url(repo)

    def git_sha_for_module(self) -> str:
        if self.args.mode != "module" and not self.args.module_path:
            return ""
        if self.args.module_sha:
            return self.args.module_sha
        if not self.args.module_path:
            return ""
        path = Path(self.args.module_path)
        repo = path.parent if path.is_file() else path
        try:
            return subprocess.check_output(
                ["git", "-C", str(repo), "rev-parse", "HEAD"],
                text=True,
                stderr=subprocess.DEVNULL,
                timeout=5,
            ).strip()
        except Exception:
            return ""

    def git_remote_for_module(self) -> str:
        if self.args.mode != "module" and not self.args.module_path:
            return ""
        if not self.args.module_path:
            return ""
        return git_remote_url(Path(self.args.module_path))

    @staticmethod
    def binary_version(binary: str) -> str:
        try:
            return subprocess.check_output(
                [binary, "--version"],
                text=True,
                stderr=subprocess.STDOUT,
                timeout=5,
            ).strip()
        except Exception:
            return ""

    def set_default_roaring(self, enabled: bool, required: Optional[bool] = None) -> None:
        if required is None:
            required = self.args.mode == "native"
        try:
            self.client.execute(["CONFIG", "SET", "bitmap-default-roaring", "yes" if enabled else "no"])
        except BenchError:
            if required:
                raise

    def convert_to_native(self, key: str) -> None:
        if self.args.mode == "native":
            self.client.execute(["BITMAP", "CONVERT", key, "NATIVE"])

    def module_cmd(self, name: str) -> str:
        return f"{self.args.module_command_prefix}.{name}"

    @staticmethod
    def module_key(key: str) -> str:
        return key if key.endswith(":module") else f"{key}:module"

    def prepare_data(self) -> None:
        if self.args.mode == "module":
            self.prepare_module_data()
            return
        print("preparing bitmap datasets...", file=sys.stderr)
        self.dataset_keys = dict(DATASET_KEYS)
        self.dataset_metadata = {}
        self.realdata = self.load_realdata()
        self.client.execute(["FLUSHDB"])
        self.set_default_roaring(False, required=False)

        dense = dense_payload(self.args.dense_bytes)
        self.client.execute(["SET", DATASET_KEYS["dense_legacy"], dense])
        self.client.execute(["SET", DATASET_KEYS["dense_native"], dense])
        self.convert_to_native(DATASET_KEYS["dense_native"])
        self.record_dataset_metadata(DATASET_KEYS["dense_legacy"], dataset_metrics_from_payload(dense))
        self.record_dataset_metadata(DATASET_KEYS["dense_native"], dataset_metrics_from_payload(dense))

        sparse_bits = self.sparse_bits()
        clustered_bits = self.clustered_bits()
        self.seed_setbit_bitmap(DATASET_KEYS["sparse_legacy"], sparse_bits, native=False)
        self.seed_setbit_bitmap(DATASET_KEYS["sparse_native"], sparse_bits, native=True)
        self.seed_setbit_bitmap(DATASET_KEYS["clustered_legacy"], clustered_bits, native=False)
        self.seed_setbit_bitmap(DATASET_KEYS["clustered_native"], clustered_bits, native=True)
        sparse_metrics = dataset_metrics_from_bits(sparse_bits, self.args.sparse_space)
        clustered_metrics = dataset_metrics_from_bits(clustered_bits)
        self.record_dataset_metadata(DATASET_KEYS["sparse_legacy"], sparse_metrics)
        self.record_dataset_metadata(DATASET_KEYS["sparse_native"], sparse_metrics)
        self.record_dataset_metadata(DATASET_KEYS["clustered_legacy"], clustered_metrics)
        self.record_dataset_metadata(DATASET_KEYS["clustered_native"], clustered_metrics)

        mixed_len = min(self.args.dense_bytes, self.args.mixed_bytes)
        mixed_a = mixed_payload(mixed_len, 11)
        mixed_b = mixed_payload(mixed_len, 37)
        mixed_c = mixed_payload(mixed_len, 71)
        mixed_d = mixed_payload(mixed_len, 109)
        self.client.execute(["SET", DATASET_KEYS["mixed_legacy_a"], mixed_a])
        self.client.execute(["SET", DATASET_KEYS["mixed_native_b"], mixed_b])
        self.convert_to_native(DATASET_KEYS["mixed_native_b"])
        self.client.execute(["SET", DATASET_KEYS["mixed_legacy_c"], mixed_c])
        self.client.execute(["SET", DATASET_KEYS["mixed_native_d"], mixed_d])
        self.convert_to_native(DATASET_KEYS["mixed_native_d"])
        self.record_dataset_metadata(DATASET_KEYS["mixed_legacy_a"], dataset_metrics_from_payload(mixed_a))
        self.record_dataset_metadata(DATASET_KEYS["mixed_native_b"], dataset_metrics_from_payload(mixed_b))
        self.record_dataset_metadata(DATASET_KEYS["mixed_legacy_c"], dataset_metrics_from_payload(mixed_c))
        self.record_dataset_metadata(DATASET_KEYS["mixed_native_d"], dataset_metrics_from_payload(mixed_d))
        self.prepare_realdata()
        self.set_default_roaring(False, required=False)

    def prepare_module_data(self) -> None:
        print("preparing redis-roaring module datasets...", file=sys.stderr)
        self.dataset_keys = {name: self.module_key(key) for name, key in DATASET_KEYS.items()}
        self.dataset_metadata = {}
        self.realdata = self.load_realdata()
        self.client.execute(["FLUSHDB"])

        dense = dense_payload(self.args.dense_bytes)
        self.seed_module_bitmap_from_payload(self.dataset_keys["dense_legacy"], dense)
        self.seed_module_bitmap_from_payload(self.dataset_keys["dense_native"], dense)
        self.record_dataset_metadata(self.dataset_keys["dense_legacy"], dataset_metrics_from_payload(dense))
        self.record_dataset_metadata(self.dataset_keys["dense_native"], dataset_metrics_from_payload(dense))

        sparse_bits = self.sparse_bits()
        clustered_bits = self.clustered_bits()
        self.seed_module_bitmap(self.dataset_keys["sparse_legacy"], sparse_bits)
        self.seed_module_bitmap(self.dataset_keys["sparse_native"], sparse_bits)
        self.seed_module_bitmap(self.dataset_keys["clustered_legacy"], clustered_bits)
        self.seed_module_bitmap(self.dataset_keys["clustered_native"], clustered_bits)
        sparse_metrics = dataset_metrics_from_bits(sparse_bits, self.args.sparse_space)
        clustered_metrics = dataset_metrics_from_bits(clustered_bits)
        self.record_dataset_metadata(self.dataset_keys["sparse_legacy"], sparse_metrics)
        self.record_dataset_metadata(self.dataset_keys["sparse_native"], sparse_metrics)
        self.record_dataset_metadata(self.dataset_keys["clustered_legacy"], clustered_metrics)
        self.record_dataset_metadata(self.dataset_keys["clustered_native"], clustered_metrics)

        mixed_len = min(self.args.dense_bytes, self.args.mixed_bytes)
        mixed_a = mixed_payload(mixed_len, 11)
        mixed_b = mixed_payload(mixed_len, 37)
        mixed_c = mixed_payload(mixed_len, 71)
        mixed_d = mixed_payload(mixed_len, 109)
        self.seed_module_bitmap_from_payload(
            self.dataset_keys["mixed_legacy_a"],
            mixed_a,
        )
        self.seed_module_bitmap_from_payload(
            self.dataset_keys["mixed_native_b"],
            mixed_b,
        )
        self.seed_module_bitmap_from_payload(
            self.dataset_keys["mixed_legacy_c"],
            mixed_c,
        )
        self.seed_module_bitmap_from_payload(
            self.dataset_keys["mixed_native_d"],
            mixed_d,
        )
        self.record_dataset_metadata(self.dataset_keys["mixed_legacy_a"], dataset_metrics_from_payload(mixed_a))
        self.record_dataset_metadata(self.dataset_keys["mixed_native_b"], dataset_metrics_from_payload(mixed_b))
        self.record_dataset_metadata(self.dataset_keys["mixed_legacy_c"], dataset_metrics_from_payload(mixed_c))
        self.record_dataset_metadata(self.dataset_keys["mixed_native_d"], dataset_metrics_from_payload(mixed_d))
        self.prepare_realdata()

    def record_dataset_metadata(self, key: str, metrics: dict[str, Any]) -> None:
        self.dataset_metadata[key] = metrics

    def seed_module_bitmap_from_payload(self, key: str, payload: bytes) -> None:
        self.seed_module_bitmap(key, iter_payload_set_bits(payload))

    def seed_module_bitmap(self, key: str, bits: Iterable[int]) -> None:
        self.client.execute(["DEL", key])

        def commands() -> Iterable[list[Any]]:
            first = True
            for chunk in chunked_ints(bits, max(1, self.args.module_seed_chunk_size)):
                if self.args.module_command_prefix == "R":
                    max_offset = max(chunk)
                    if max_offset > 0xFFFFFFFF:
                        raise BenchError(
                            "redis-roaring R.* mode only supports offsets up to UINT32_MAX; "
                            "use --module-command-prefix R64 for larger datasets"
                        )
                command = "SETINTARRAY" if first else "APPENDINTARRAY"
                first = False
                yield [self.module_cmd(command), key, *chunk]

        self.client.pipeline(commands(), chunk_size=16)

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

    def load_realdata(self) -> list[RealDataSet]:
        if not self.args.croaring_realdata_dir and not self.args.download_croaring_realdata:
            return []
        if self.args.realdata_max_files <= 0:
            return []
        realdata_dir = self.ensure_realdata_dir()
        datasets: list[RealDataSet] = []
        sources = []
        for path in sorted(realdata_dir.rglob("*")):
            if path.is_dir() or path.name == "bitsets_1925630_96.gz":
                continue
            if path.suffix.lower() == ".zip" or path.suffix.lower() in ("", ".txt", ".csv"):
                sources.append(path)

        for index, path in enumerate(sources):
            remaining_slots = self.args.realdata_max_files - len(datasets)
            if remaining_slots <= 0:
                break
            remaining_sources = len(sources) - index
            source_limit = max(1, (remaining_slots + remaining_sources - 1) // remaining_sources)
            if path.suffix.lower() == ".zip":
                datasets.extend(self.load_realdata_zip(path, source_limit))
            elif path.suffix.lower() in ("", ".txt", ".csv"):
                bits = parse_realdata_text(path.read_text(encoding="utf-8", errors="replace"),
                                           self.args.realdata_max_values)
                if bits:
                    datasets.append(RealDataSet(sanitize_name(path.stem), bits))
        return datasets[:self.args.realdata_max_files]

    def ensure_realdata_dir(self) -> Path:
        if self.args.croaring_realdata_dir:
            realdata_dir = Path(self.args.croaring_realdata_dir)
        else:
            realdata_dir = Path(tempfile.mkdtemp(prefix="bitmap-realdata-"))
        realdata_dir.mkdir(parents=True, exist_ok=True)
        if self.args.download_croaring_realdata:
            wanted = self.args.realdata_archives or ",".join(REALDATA_ARCHIVES)
            for name in [item.strip() for item in wanted.split(",") if item.strip()]:
                archive = realdata_dir / f"{name}.zip"
                if archive.exists():
                    continue
                url = f"{REALDATA_BASE_URL}/{name}.zip"
                print(f"downloading {url}", file=sys.stderr)
                urlretrieve(url, archive)
        return realdata_dir

    def load_realdata_zip(self, path: Path, limit: int) -> list[RealDataSet]:
        datasets: list[RealDataSet] = []
        if limit <= 0:
            return datasets
        with ZipFile(path) as zf:
            for member in sorted(zf.namelist()):
                if member.endswith("/"):
                    continue
                with zf.open(member) as fp:
                    text = fp.read().decode("utf-8", "replace")
                bits = parse_realdata_text(text, self.args.realdata_max_values)
                if bits:
                    name = sanitize_name(f"{path.stem}_{Path(member).stem}")
                    datasets.append(RealDataSet(name, bits))
                if len(datasets) >= limit:
                    break
        return datasets

    def seed_setbit_bitmap(self, key: str, bits: list[int], native: bool) -> None:
        native = native and self.args.mode == "native"
        self.client.execute(["DEL", key])
        self.set_default_roaring(native, required=native)
        if not bits:
            self.client.execute(["SET", key, b""])
            if native:
                self.convert_to_native(key)
            return
        self.client.pipeline((["SETBIT", key, bit, "1"] for bit in bits))
        self.set_default_roaring(False, required=False)

    def prepare_realdata(self) -> None:
        for item in self.realdata:
            legacy_key = f"bench:bitmap:real:{item.name}:legacy"
            native_key = f"bench:bitmap:real:{item.name}:native"
            if self.args.mode == "module":
                legacy_key = self.module_key(legacy_key)
                native_key = self.module_key(native_key)
                self.seed_module_bitmap(legacy_key, item.bits)
                self.seed_module_bitmap(native_key, item.bits)
            else:
                self.seed_setbit_bitmap(legacy_key, item.bits, native=False)
                self.seed_setbit_bitmap(native_key, item.bits, native=True)
            self.dataset_keys[f"real_{item.name}_legacy"] = legacy_key
            self.dataset_keys[f"real_{item.name}_native"] = native_key
            metrics = dataset_metrics_from_bits(item.bits)
            self.record_dataset_metadata(legacy_key, metrics)
            self.record_dataset_metadata(native_key, metrics)

    def dataset_summary(self) -> list[DatasetSummary]:
        summaries = []
        for name, key in self.dataset_keys.items():
            bitcount_cmd = self.module_cmd("BITCOUNT") if self.args.mode == "module" else "BITCOUNT"
            metadata = self.dataset_metadata.get(key, {})
            summaries.append(DatasetSummary(
                name=name,
                key=key,
                redis_type=decode_text(self.client.execute(["TYPE", key])),
                encoding=decode_text(self.client.execute(["OBJECT", "ENCODING", key])),
                bitcount=int(self.client.execute([bitcount_cmd, key])),
                memory_usage_bytes=self.memory_usage(key),
                dump_payload_bytes=self.dump_payload_size(key),
                max_offset=metadata.get("max_offset"),
                logical_bytes=metadata.get("logical_bytes"),
                density=metadata.get("density"),
            ))
        return summaries

    def selected_workloads(self) -> list[Workload]:
        workloads = self.workloads()
        all_names = {w.name for w in workloads}
        if not self.args.only:
            workloads = self.filter_workloads_by_profile(workloads)
        if self.args.mode == "module":
            module_workloads = []
            for workload in workloads:
                translated = self.module_workload(workload)
                if translated is not None:
                    module_workloads.append(translated)
            workloads = module_workloads
        elif self.args.mode != "native":
            workloads = [w for w in workloads if not w.native_only]
        if not self.args.only:
            return workloads
        wanted = {name.strip() for name in self.args.only.split(",") if name.strip()}
        unknown = wanted - all_names
        if unknown:
            raise BenchError(f"unknown workload(s): {', '.join(sorted(unknown))}")
        unsupported = wanted - {w.name for w in workloads}
        if unsupported:
            raise BenchError(
                f"workload(s) not supported in {self.args.mode} mode: "
                f"{', '.join(sorted(unsupported))}"
            )
        return [w for w in workloads if w.name in wanted]

    def filter_workloads_by_profile(self, workloads: list[Workload]) -> list[Workload]:
        if self.args.benchmark_profile == "full":
            return workloads
        if self.args.benchmark_profile == "smoke":
            return [w for w in workloads if w.name in SMOKE_WORKLOADS]
        groups = set(PROFILE_GROUPS[self.args.benchmark_profile])
        return [w for w in workloads if groups.intersection(w.target_groups)]

    def module_workload(self, workload: Workload) -> Optional[Workload]:
        if workload.native_only:
            return None
        command = self.module_command_for(workload.command)
        if command is None:
            return None
        sample_key = self.module_key(workload.sample_key) if workload.sample_key else None
        return replace(workload, command=command, sample_key=sample_key)

    def module_command_for(self, command: list[str]) -> Optional[list[str]]:
        if not command:
            return None
        name = command[0].upper()
        if name in ("SETBIT", "GETBIT"):
            return [self.module_cmd(name), self.module_key(command[1]), *command[2:]]
        if name == "BITCOUNT":
            if len(command) != 2:
                return None
            return [self.module_cmd("BITCOUNT"), self.module_key(command[1])]
        if name == "BITPOS":
            if len(command) != 3:
                return None
            return [self.module_cmd("BITPOS"), self.module_key(command[1]), command[2]]
        if name == "BITOP":
            operation = command[1].upper()
            if operation not in ("AND", "OR", "XOR", "NOT", "DIFF", "DIFF1", "ANDOR", "ONE"):
                return None
            return [
                self.module_cmd("BITOP"),
                operation,
                self.module_key(command[2]),
                *(self.module_key(key) for key in command[3:]),
            ]
        return None

    def workloads(self) -> list[Workload]:
        sparse_hit_offset = str(self.sparse_bits()[-1])
        workloads = [
            Workload(
                "setbit_native_create_sparse",
                "One-shot SETBIT latency creating a missing sparse key as a native bitmap",
                ["SETBIT", "bench:bitmap:setbit:create", str(self.args.sparse_space - 1), "1"],
                1, 1, 1,
                warmup_requests=0, setup="setup_setbit_native_create",
                sample_key="bench:bitmap:setbit:create",
                one_shot=True, story="Sparse key creation", dataset="synthetic_sparse",
                target_groups=("small-sets",),
                stall_probe=True,
            ),
            Workload(
                "setbit_convert_string_dense",
                "One-shot SETBIT latency converting an existing legacy string bitmap to native before writing",
                ["SETBIT", "bench:bitmap:setbit:convert", str(self.args.convert_bytes * 8 - 1), "1"],
                1, 1, 1,
                warmup_requests=0, setup="setup_setbit_convert_string",
                sample_key="bench:bitmap:setbit:convert",
                one_shot=True, story="String-to-native migration",
                dataset="synthetic_dense", target_groups=("bitsets",),
                native_only=True, stall_probe=True,
            ),
            Workload(
                "getbit_sparse_native_hit",
                "GETBIT hit against a sparse native bitmap",
                ["GETBIT", DATASET_KEYS["sparse_native"], sparse_hit_offset],
                80_000, 32, 16, sample_key=DATASET_KEYS["sparse_native"],
                story="Membership lookup", dataset="synthetic_sparse",
                target_groups=("small-sets",),
            ),
            Workload(
                "bitcount_dense_legacy",
                "BITCOUNT over a dense legacy string bitmap",
                ["BITCOUNT", DATASET_KEYS["dense_legacy"]],
                50_000, 32, 8, sample_key=DATASET_KEYS["dense_legacy"],
                story="Count a dense cohort", dataset="synthetic_dense",
                target_groups=("bitsets",),
            ),
            Workload(
                "bitcount_dense_native",
                "BITCOUNT over a dense native bitmap",
                ["BITCOUNT", DATASET_KEYS["dense_native"]],
                50_000, 32, 8, sample_key=DATASET_KEYS["dense_native"],
                story="Count a dense cohort", dataset="synthetic_dense",
                target_groups=("bitsets",),
            ),
            Workload(
                "bitcount_dense_native_range",
                "Ranged BITCOUNT over a dense native bitmap",
                ["BITCOUNT", DATASET_KEYS["dense_native"], "0", str(max(0, self.args.dense_bytes // 2))],
                50_000, 32, 8, sample_key=DATASET_KEYS["dense_native"],
                story="Count a segment", dataset="synthetic_dense",
                target_groups=("bitsets",),
            ),
            Workload(
                "bitcount_sparse_native",
                "BITCOUNT over a sparse native bitmap",
                ["BITCOUNT", DATASET_KEYS["sparse_native"]],
                50_000, 32, 8, sample_key=DATASET_KEYS["sparse_native"],
                story="Count a sparse cohort", dataset="synthetic_sparse",
                target_groups=("small-sets",),
            ),
            Workload(
                "bitpos_clustered_native",
                "BITPOS over clustered native runs",
                ["BITPOS", DATASET_KEYS["clustered_native"], "1"],
                40_000, 24, 8, sample_key=DATASET_KEYS["clustered_native"],
                story="First match", dataset="synthetic_clustered",
                target_groups=("bitsets",),
            ),
            Workload(
                "bitpos_zero_clustered_native",
                "BITPOS 0 over clustered native runs",
                ["BITPOS", DATASET_KEYS["clustered_native"], "0"],
                40_000, 24, 8, sample_key=DATASET_KEYS["clustered_native"],
                story="First gap", dataset="synthetic_clustered",
                target_groups=("bitsets",),
            ),
            Workload(
                "bitfield_ro_native_hot",
                "BITFIELD_RO GET over a clustered native offset",
                ["BITFIELD_RO", DATASET_KEYS["clustered_native"], "GET", "u8", "0"],
                80_000, 32, 16,
                sample_key=DATASET_KEYS["clustered_native"],
                story="Packed field reads", dataset="synthetic_clustered",
                target_groups=("bitsets",),
            ),
            Workload(
                "bitfield_set_native_hot",
                "BITFIELD SET writes into a native bitmap",
                ["BITFIELD", "bench:bitmap:bitfield:write", "SET", "u1", str(self.args.sparse_space - 1), "1"],
                50_000, 24, 8,
                setup="setup_bitfield_native_write",
                sample_key="bench:bitmap:bitfield:write",
                story="Packed field writes", dataset="synthetic_sparse",
                target_groups=("bitsets",),
            ),
            Workload(
                "bitop_and_all_string",
                "BITOP AND with all string bitmap sources",
                ["BITOP", "AND", "bench:bitmap:bitop:and:string:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_legacy_c"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:and:string:dest",
                story="Audience intersection", dataset="synthetic_mixed_all_string",
                target_groups=("mixed-bitop",),
            ),
            Workload(
                "bitop_and_all_native",
                "BITOP AND with all native bitmap sources",
                ["BITOP", "AND", "bench:bitmap:bitop:and:native:dest",
                 DATASET_KEYS["mixed_native_b"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:and:native:dest",
                story="Audience intersection", dataset="synthetic_mixed_all_native",
                target_groups=("mixed-bitop",),
            ),
            Workload(
                "bitop_and_mixed",
                "BITOP AND with mixed legacy string and native bitmap sources",
                ["BITOP", "AND", "bench:bitmap:bitop:and:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:and:dest",
                story="Audience intersection", dataset="synthetic_mixed",
                target_groups=("mixed-bitop",),
            ),
            Workload(
                "bitop_or_mixed",
                "BITOP OR with mixed legacy string and native bitmap sources",
                ["BITOP", "OR", "bench:bitmap:bitop:or:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:or:dest",
                story="Reach / union", dataset="synthetic_mixed",
                target_groups=("mixed-bitop",),
            ),
            Workload(
                "bitop_xor_mixed",
                "BITOP XOR with mixed legacy string and native bitmap sources",
                ["BITOP", "XOR", "bench:bitmap:bitop:xor:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:xor:dest",
                story="Symmetric difference", dataset="synthetic_mixed",
                target_groups=("mixed-bitop",),
            ),
            Workload(
                "bitop_not_native",
                "BITOP NOT from a native bitmap source",
                ["BITOP", "NOT", "bench:bitmap:bitop:not:dest", DATASET_KEYS["clustered_native"]],
                10_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:not:dest",
                story="Exclusion / suppression", dataset="synthetic_clustered",
                target_groups=("mixed-bitop",),
            ),
            Workload(
                "bitop_diff1_mixed",
                "BITOP DIFF1 over mixed legacy string and native bitmap sources",
                ["BITOP", "DIFF1", "bench:bitmap:bitop:diff1:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:diff1:dest",
                story="Difference / suppression", dataset="synthetic_mixed",
                target_groups=("mixed-bitop",),
            ),
            Workload(
                "bitop_one_mixed",
                "BITOP ONE over mixed legacy string and native bitmap sources",
                ["BITOP", "ONE", "bench:bitmap:bitop:one:dest",
                 DATASET_KEYS["mixed_legacy_a"], DATASET_KEYS["mixed_native_b"],
                 DATASET_KEYS["mixed_legacy_c"], DATASET_KEYS["mixed_native_d"]],
                12_000, 12, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:bitop:one:dest",
                story="Exactly-one filter", dataset="synthetic_mixed",
                target_groups=("mixed-bitop",),
            ),
        ]
        workloads.extend(self.realdata_workloads())
        return workloads

    def realdata_workloads(self) -> list[Workload]:
        workloads: list[Workload] = []
        for item in self.realdata:
            native_key = f"bench:bitmap:real:{item.name}:native"
            workloads.append(Workload(
                f"bitcount_realdata_{item.name}",
                f"BITCOUNT over CRoaring realdata dataset {item.name}",
                ["BITCOUNT", native_key],
                30_000, 24, 8, sample_key=native_key,
                story="Count a realdata cohort", dataset=item.name,
                target_groups=("small-sets",),
            ))
        if len(self.realdata) >= 2:
            a = self.realdata[0].name
            b = self.realdata[1].name
            workloads.append(Workload(
                f"bitop_and_realdata_{a}_{b}",
                f"BITOP AND over CRoaring realdata datasets {a} and {b}",
                ["BITOP", "AND", "bench:bitmap:real:and:dest",
                 f"bench:bitmap:real:{a}:native", f"bench:bitmap:real:{b}:native"],
                8_000, 8, 4, setup="setup_bitop_mixed",
                sample_key="bench:bitmap:real:and:dest",
                story="Realdata intersection", dataset=f"{a}+{b}",
                target_groups=("small-sets", "mixed-bitop"),
            ))
        return workloads

    def setup_setbit_native_create(self) -> None:
        if self.args.mode == "module":
            self.client.execute(["DEL", self.module_key("bench:bitmap:setbit:create")])
            return
        self.set_default_roaring(self.args.mode == "native", required=self.args.mode == "native")
        self.client.execute(["DEL", "bench:bitmap:setbit:create"])

    def setup_setbit_convert_string(self) -> None:
        self.set_default_roaring(False, required=False)
        self.client.execute(["DEL", "bench:bitmap:setbit:convert"])
        self.client.execute(["SET", "bench:bitmap:setbit:convert", dense_payload(self.args.convert_bytes)])
        self.set_default_roaring(True, required=True)

    def setup_bitfield_native_write(self) -> None:
        self.set_default_roaring(False, required=False)
        self.client.execute(["DEL", "bench:bitmap:bitfield:write"])
        self.client.execute(["SET", "bench:bitmap:bitfield:write", b""])
        self.convert_to_native("bench:bitmap:bitfield:write")

    def setup_bitop_mixed(self) -> None:
        keys = [
            "bench:bitmap:bitop:and:string:dest",
            "bench:bitmap:bitop:and:native:dest",
            "bench:bitmap:bitop:and:dest",
            "bench:bitmap:bitop:or:dest",
            "bench:bitmap:bitop:xor:dest",
            "bench:bitmap:bitop:not:dest",
            "bench:bitmap:bitop:diff1:dest",
            "bench:bitmap:bitop:one:dest",
            "bench:bitmap:real:and:dest",
        ]
        if self.args.mode == "module":
            keys = [self.module_key(key) for key in keys]
        self.set_default_roaring(False, required=False)
        self.client.execute(["DEL", *keys])

    def run_workload(self, workload: Workload) -> Result:
        sample_count = max(1, self.args.runs)
        if workload.one_shot:
            sample_count = max(sample_count, self.args.one_shot_min_runs)
        samples = [
            self.run_workload_sample(workload, run_index)
            for run_index in range(sample_count)
        ]
        return self.aggregate_samples(workload, samples)

    def run_workload_sample(self, workload: Workload, run_index: int) -> Result:
        if workload.setup:
            getattr(self, workload.setup)()
        if workload.one_shot:
            return self.run_one_shot_workload(workload, run_index)
        if self.args.warmup and workload.warmup_requests > 0:
            self.invoke_benchmark(workload, self.scale_requests(workload.warmup_requests))

        before = self.memory_snapshot()
        start = time.perf_counter()
        raw = self.invoke_benchmark(workload, self.scale_requests(workload.requests))
        elapsed = elapsed_ms(start)
        after = self.memory_snapshot()
        qps = self.parse_qps(raw)
        if qps <= 0:
            qps = None
        key = workload.sample_key
        requests = self.scale_requests(workload.requests)
        per_op = time_per_op_us(elapsed, requests, qps)
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
            requests=requests,
            clients=workload.clients,
            pipeline=workload.pipeline,
            rand_range=workload.rand_range,
            command=workload.command,
            time_per_op_us=per_op,
            extra=self.result_extra(workload, run_index, {"raw_output": raw.strip()}),
        )

    def run_one_shot_workload(self, workload: Workload, run_index: int) -> Result:
        before = self.memory_snapshot()
        response = None
        with PingCanary(
            self.client,
            self.args.ping_canary_interval,
            self.args.ping_canary and workload.stall_probe,
        ) as canary:
            with self.client.connection() as conn:
                start = time.perf_counter()
                response = conn.command(workload.command)
                elapsed = elapsed_ms(start)
        after = self.memory_snapshot()
        key = workload.sample_key
        extra = {
            "measurement": "one-shot latency",
            "response": decode_text(response),
        }
        stall = canary.stats()
        if stall is not None:
            extra["stall"] = asdict(stall)
        per_op = time_per_op_us(elapsed, 1, None)
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
            time_per_op_us=per_op,
            extra=self.result_extra(workload, run_index, extra),
        )

    def result_extra(self, workload: Workload, run_index: int, extra: dict[str, Any]) -> dict[str, Any]:
        payload = {
            "mode": self.args.mode,
            "mode_label": self.args.mode_label,
        }
        if run_index >= 0:
            payload["run"] = run_index + 1
        else:
            payload["aggregation"] = "median"
        if workload.story:
            payload["story"] = workload.story
        if workload.dataset:
            payload["dataset"] = workload.dataset
        payload["target_groups"] = list(workload.target_groups)
        payload.update(extra)
        return payload

    def aggregate_samples(self, workload: Workload, samples: list[Result]) -> Result:
        if len(samples) == 1:
            return samples[0]
        qps_values = [sample.qps for sample in samples if sample.qps is not None]
        elapsed_values = [sample.elapsed_ms for sample in samples]
        per_op_values = [sample.time_per_op_us for sample in samples if sample.time_per_op_us is not None]
        per_op_stats = sample_stats(per_op_values)
        extra = self.result_extra(workload, -1, {
            "runs": len(samples),
            "elapsed_ms_min": min(elapsed_values),
            "elapsed_ms_max": max(elapsed_values),
            "time_per_op_us_mean": per_op_stats["mean"],
            "time_per_op_us_median": per_op_stats["median"],
            "time_per_op_us_min": per_op_stats["min"],
            "time_per_op_us_max": per_op_stats["max"],
            "time_per_op_us_stdev": per_op_stats["stdev"],
            "time_per_op_us_cv_percent": per_op_stats["cv_percent"],
            "samples": [asdict(sample) for sample in samples],
        })
        if qps_values:
            extra["qps_min"] = min(qps_values)
            extra["qps_max"] = max(qps_values)
        return Result(
            name=workload.name,
            category=samples[0].category,
            description=workload.description,
            elapsed_ms=statistics.median(elapsed_values),
            memory_usage_bytes=median_optional(sample.memory_usage_bytes for sample in samples),
            used_memory_before=median_optional(sample.used_memory_before for sample in samples),
            used_memory_after=median_optional(sample.used_memory_after for sample in samples),
            used_memory_peak=median_optional(sample.used_memory_peak for sample in samples),
            payload_size_bytes=median_optional(sample.payload_size_bytes for sample in samples),
            qps=statistics.median(qps_values) if qps_values else None,
            requests=samples[0].requests,
            clients=samples[0].clients,
            pipeline=samples[0].pipeline,
            rand_range=samples[0].rand_range,
            command=samples[0].command,
            time_per_op_us=per_op_stats["median"],
            extra=extra,
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
        scaled = max(1, int(requests * self.args.request_scale))
        if requests > 1:
            return max(self.args.min_requests, scaled)
        return scaled

    def run_persistence_suite(self) -> None:
        print("\nrunning persistence benchmarks...", file=sys.stderr)
        self.prepare_data()
        for name in ("dense_legacy", "dense_native", "sparse_native", "clustered_native",
                     "mixed_legacy_a", "mixed_native_b"):
            self.persistence_results.append(self.run_dump_restore(name, self.dataset_keys[name]))

        self.prepare_data()
        self.persistence_results.append(self.run_rdb_save_load())
        self.persistence_results.append(self.run_aof_rewrite())

    def run_dump_restore(self, name: str, key: str) -> Result:
        before = self.memory_snapshot()
        dest = f"{key}:restored"
        with PingCanary(self.client, self.args.ping_canary_interval, self.args.ping_canary) as canary:
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
        extra = {
            "dump_ms": dump_elapsed,
            "restore_ms": restore_elapsed,
            "restored_type": decode_text(self.client.execute(["TYPE", dest])),
            "restored_encoding": decode_text(self.client.execute(["OBJECT", "ENCODING", dest])),
        }
        if self.args.mode == "module":
            extra["payload_format"] = "redis-roaring-module"
        stall = canary.stats()
        if stall is not None:
            extra["stall"] = asdict(stall)
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
            extra=extra,
        )

    def run_rdb_save_load(self) -> Result:
        before = self.memory_snapshot()
        with PingCanary(self.client, self.args.ping_canary_interval, self.args.ping_canary) as canary:
            started = time.perf_counter()
            self.client.execute(["SAVE"])
            save_elapsed = elapsed_ms(started)
            rdb_path = self.require_data_dir() / "dump.rdb"
            payload_size = rdb_path.stat().st_size if rdb_path.exists() else None
            load_elapsed = self.restart_server_for_rdb_load()
        after = self.memory_snapshot()
        extra = {
            "save_ms": save_elapsed,
            "load_ms": load_elapsed,
            "dbsize_after_load": int(self.client.execute(["DBSIZE"])),
        }
        if self.args.mode == "module":
            extra["payload_format"] = "redis-roaring-module"
        stall = canary.stats()
        if stall is not None:
            extra["stall"] = asdict(stall)
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
            extra=extra,
        )

    def run_aof_rewrite(self) -> Result:
        before = self.memory_snapshot()
        self.client.execute(["CONFIG", "SET", "appendonly", "yes"])
        self.wait_for_aof_rewrite()
        with PingCanary(self.client, self.args.ping_canary_interval, self.args.ping_canary) as canary:
            started = time.perf_counter()
            self.client.execute(["BGREWRITEAOF"])
            self.wait_for_aof_rewrite()
            rewrite_elapsed = elapsed_ms(started)
        persistence = parse_info(self.client.execute(["INFO", "PERSISTENCE"]))
        aof_current_size = to_int(persistence.get("aof_current_size"))
        after = self.memory_snapshot()
        extra = {
            "aof_last_bgrewrite_status": persistence.get("aof_last_bgrewrite_status"),
            "aof_current_size": aof_current_size,
        }
        if self.args.mode == "module":
            extra["payload_format"] = "redis-roaring-module"
        stall = canary.stats()
        if stall is not None:
            extra["stall"] = asdict(stall)
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
            extra=extra,
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
        print("| name | type | encoding | bitcount | max offset | logical bytes | density | memory bytes | dump bytes |")
        print("|---|---|---|---:|---:|---:|---:|---:|---:|")
        for item in self.datasets:
            print(
                f"| {item.name} | {item.redis_type} | {item.encoding} | "
                f"{item.bitcount} | {fmt_optional(item.max_offset)} | "
                f"{fmt_optional(item.logical_bytes)} | {fmt_ratio(item.density)} | "
                f"{fmt_optional(item.memory_usage_bytes)} | "
                f"{fmt_optional(item.dump_payload_bytes)} |"
            )

    def print_summary(self) -> None:
        print("\ncommand summary:")
        print("| story | groups | workload | time/op us | stdev us | qps | elapsed/latency ms | memory bytes | peak bytes | stall ms | payload bytes |")
        print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
        for result in self.results:
            qps = "N/A" if result.qps is None else f"{result.qps:.2f}"
            target_groups = ",".join(result.extra.get("target_groups", [])) or "-"
            stall = result_stall_ms(result)
            print(
                f"| {result.extra.get('story', '-')} | {target_groups} | {result.name} | "
                f"{fmt_float(result.time_per_op_us)} | {fmt_float(result.extra.get('time_per_op_us_stdev'))} | "
                f"{qps} | {result.elapsed_ms:.2f} | "
                f"{fmt_optional(result.memory_usage_bytes)} | "
                f"{fmt_optional(result.used_memory_peak)} | "
                f"{fmt_float(stall)} | "
                f"{fmt_optional(result.payload_size_bytes)} |"
            )

        if self.persistence_results:
            print("\npersistence summary:")
            print("| operation | elapsed ms | memory bytes | peak bytes | stall ms | payload bytes | notes |")
            print("|---|---:|---:|---:|---:|---:|---|")
            for result in self.persistence_results:
                notes = ", ".join(f"{k}={v}" for k, v in result.extra.items() if k.endswith("_ms") or k.endswith("_status"))
                print(
                    f"| {result.name} | {result.elapsed_ms:.2f} | "
                    f"{fmt_optional(result.memory_usage_bytes)} | "
                    f"{fmt_optional(result.used_memory_peak)} | "
                    f"{fmt_float(result_stall_ms(result))} | "
                    f"{fmt_optional(result.payload_size_bytes)} | {notes} |"
                )

    def write_json(self) -> None:
        payload = {
            "mode": self.args.mode,
            "mode_label": self.args.mode_label,
            "host": self.host,
            "port": self.port,
            "db": self.db,
            "environment": self.environment,
            "parameters": {
                "benchmark_profile": self.args.benchmark_profile,
                "report_view": self.args.report_view,
                "dense_bytes": self.args.dense_bytes,
                "sparse_space": self.args.sparse_space,
                "sparse_count": self.args.sparse_count,
                "cluster_count": self.args.cluster_count,
                "cluster_span": self.args.cluster_span,
                "cluster_gap": self.args.cluster_gap,
                "request_scale": self.args.request_scale,
                "min_requests": self.args.min_requests,
                "runs": self.args.runs,
                "one_shot_min_runs": self.args.one_shot_min_runs,
            },
            "datasets": [asdict(d) for d in self.datasets],
            "results": [asdict(r) for r in self.results],
            "persistence": [asdict(r) for r in self.persistence_results],
        }
        with open(self.args.json_out, "w", encoding="utf-8") as fp:
            json.dump(payload, fp, indent=2)
        print(f"json written to {self.args.json_out}")

    def write_csv(self) -> None:
        rows = [self.result_row(result) for result in self.results + self.persistence_results]
        with open(self.args.csv_out, "w", newline="", encoding="utf-8") as fp:
            writer = csv.DictWriter(fp, fieldnames=list(rows[0].keys()) if rows else CSV_FIELDS)
            writer.writeheader()
            writer.writerows(rows)
        print(f"csv written to {self.args.csv_out}")

    def write_markdown(self) -> None:
        lines = self.markdown_lines()
        Path(self.args.markdown_out).write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"markdown written to {self.args.markdown_out}")

    def markdown_lines(self) -> list[str]:
        lines = [
            f"# Redis Native Bitmap Benchmark ({self.args.mode_label})",
            "",
            f"- Mode: `{self.args.mode}`",
            f"- Benchmark profile: `{self.args.benchmark_profile}`",
            f"- Report view: `{self.args.report_view}`",
            f"- Redis: `{self.environment.get('redis_version', 'unknown')}`",
            f"- Source SHA: `{self.environment.get('source_sha', 'unknown')}`",
            f"- Runner: `{self.environment.get('runner_name', 'local')}`",
            "",
            "| Dataset | Type | Encoding | Bitcount | Max Offset | Logical Bytes | Density | Memory | Payload |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for item in self.datasets:
            lines.append(
                f"| {item.name} | {item.redis_type} | {item.encoding} | {item.bitcount} | "
                f"{fmt_optional(item.max_offset)} | {fmt_optional(item.logical_bytes)} | "
                f"{fmt_ratio(item.density)} | {fmt_optional(item.memory_usage_bytes)} | "
                f"{fmt_optional(item.dump_payload_bytes)} |"
            )
        lines.extend([
            "",
            "| Story | Dataset | Groups | Workload | Metric | Value | Time/Op us | Mean us | Median us | Min us | Max us | Stdev us | CV % | QPS | Memory | Peak | Payload | Stall | Notes |",
            "| --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
        ])
        for result in self.results + self.persistence_results:
            row = self.result_row(result)
            lines.append(
                f"| {row['story']} | {row['dataset']} | {row['target_groups']} | {row['workload']} | "
                f"{row['metric']} | {row['value']} | {row['time_per_op_us']} | "
                f"{row['time_per_op_mean_us']} | {row['time_per_op_median_us']} | "
                f"{row['time_per_op_min_us']} | {row['time_per_op_max_us']} | "
                f"{row['time_per_op_stdev_us']} | {row['time_per_op_cv_percent']} | "
                f"{row['qps']} | {row['memory_bytes']} | {row['peak_bytes']} | "
                f"{row['payload_bytes']} | {row['stall_ms']} | {row['notes']} |"
            )
        return lines

    def result_row(self, result: Result) -> dict[str, Any]:
        metric = "time_per_op_us" if result.time_per_op_us is not None else "elapsed_ms"
        value = f"{result.time_per_op_us:.2f}" if result.time_per_op_us is not None else f"{result.elapsed_ms:.2f}"
        def time_stat(name: str, fallback_to_primary: bool = False) -> Optional[float]:
            stat = result.extra.get(f"time_per_op_us_{name}")
            if stat is None and fallback_to_primary:
                return result.time_per_op_us
            return stat
        notes = []
        if result.extra.get("runs"):
            notes.append(f"runs={result.extra['runs']}")
        if result.category == "persistence":
            notes.extend(f"{k}={v}" for k, v in result.extra.items() if k.endswith("_ms") or k.endswith("_status"))
        if result.extra.get("payload_format"):
            notes.append(f"payload_format={result.extra['payload_format']}")
        return {
            "mode": self.args.mode_label,
            "target_groups": ",".join(result.extra.get("target_groups", [])) or "-",
            "category": result.category,
            "story": result.extra.get("story", result.category),
            "dataset": result.extra.get("dataset", "-"),
            "workload": result.name,
            "metric": metric,
            "value": value,
            "elapsed_ms": f"{result.elapsed_ms:.2f}",
            "time_per_op_us": fmt_float(result.time_per_op_us),
            "time_per_op_mean_us": fmt_float(time_stat("mean", True)),
            "time_per_op_median_us": fmt_float(time_stat("median", True)),
            "time_per_op_min_us": fmt_float(time_stat("min", True)),
            "time_per_op_max_us": fmt_float(time_stat("max", True)),
            "time_per_op_stdev_us": fmt_float(result.extra.get("time_per_op_us_stdev")),
            "time_per_op_cv_percent": fmt_float(result.extra.get("time_per_op_us_cv_percent")),
            "qps": "N/A" if result.qps is None else f"{result.qps:.2f}",
            "memory_bytes": fmt_optional(result.memory_usage_bytes),
            "peak_bytes": fmt_optional(result.used_memory_peak),
            "payload_bytes": fmt_optional(result.payload_size_bytes),
            "stall_ms": fmt_float(result_stall_ms(result)),
            "notes": "; ".join(notes) if notes else "-",
        }


def elapsed_ms(started: float) -> float:
    return (time.perf_counter() - started) * 1000.0


def git_remote_url(repo_or_path: Path) -> str:
    repo = repo_or_path.parent if repo_or_path.is_file() else repo_or_path
    try:
        remote = subprocess.check_output(
            ["git", "-C", str(repo), "config", "--get", "remote.origin.url"],
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=5,
        ).strip()
    except Exception:
        return ""
    return normalize_git_remote_url(remote)


def normalize_git_remote_url(remote: str) -> str:
    remote = remote.strip()
    if not remote:
        return ""
    if remote.startswith("git@github.com:"):
        remote = "https://github.com/" + remote[len("git@github.com:"):]
    elif remote.startswith("ssh://git@github.com/"):
        remote = "https://github.com/" + remote[len("ssh://git@github.com/"):]
    if remote.endswith(".git"):
        remote = remote[:-4]
    return remote.rstrip("/")


def git_commit_url(repo_url: str, sha: str) -> str:
    if not repo_url or not sha:
        return ""
    if "github.com/" not in repo_url:
        return ""
    return f"{repo_url}/commit/{sha}"


def short_sha(sha: str) -> str:
    return sha[:12] if sha else ""


def markdown_escape(text: str) -> str:
    return str(text).replace("|", "\\|")


def markdown_commit_link(label: str, repo_url: str, sha: str) -> str:
    label = markdown_escape(label)
    url = git_commit_url(repo_url, sha)
    if url:
        return f"[{label}]({url})"
    if sha:
        return f"{label}@`{short_sha(sha)}`"
    return label


def github_run_url(env: dict[str, Any]) -> str:
    repository = env.get("github_repository", "")
    run_id = env.get("github_run_id", "")
    if not repository or not run_id:
        return ""
    server_url = env.get("github_server_url", "https://github.com").rstrip("/")
    return f"{server_url}/{repository}/actions/runs/{run_id}"


def markdown_runner_link(env: dict[str, Any]) -> str:
    runner = markdown_escape(env.get("runner_name", "local"))
    url = github_run_url(env)
    if url:
        return f"[{runner}]({url})"
    return runner


def to_int(value: Optional[str]) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def fmt_optional(value: Optional[int]) -> str:
    return "-" if value is None else str(value)


def fmt_float(value: Optional[float]) -> str:
    return "-" if value is None else f"{value:.2f}"


def fmt_ratio(value: Optional[float]) -> str:
    return "-" if value is None else f"{value:.6f}"


def median_optional(values: Iterable[Optional[int]]) -> Optional[int]:
    present = [value for value in values if value is not None]
    if not present:
        return None
    return int(statistics.median(present))


def parse_realdata_text(text: str, max_values: int) -> list[int]:
    bits: list[int] = []
    for match in INTEGER_RE.finditer(text):
        bits.append(int(match.group(0)))
        if len(bits) >= max_values:
            break
    return bits


def sanitize_name(name: str) -> str:
    clean = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_")
    return clean[:96] or "dataset"


def result_stall_ms(result: Result) -> Optional[float]:
    stall = result.extra.get("stall")
    if isinstance(stall, dict):
        ping = stall.get("max_ping_ms")
        gap = stall.get("max_gap_ms")
        values = [value for value in (ping, gap) if isinstance(value, (int, float))]
        return max(values) if values else None
    samples = result.extra.get("samples")
    if isinstance(samples, list):
        values = []
        for sample in samples:
            if isinstance(sample, dict):
                sample_stall = sample.get("extra", {}).get("stall")
                if isinstance(sample_stall, dict):
                    for key in ("max_ping_ms", "max_gap_ms"):
                        value = sample_stall.get(key)
                        if isinstance(value, (int, float)):
                            values.append(float(value))
        return max(values) if values else None
    return None


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
    parser.add_argument("--mode", choices=("native", "legacy", "module"), default="native",
                        help=("native uses bitmap-default-roaring/convert; legacy uses string bitmap data only; "
                              "module uses redis-roaring R.* commands"))
    parser.add_argument("--mode-label", default="redis-pr-native",
                        help="Label written to JSON/CSV/Markdown output")
    parser.add_argument("--module-path",
                        help="Path to libredis-roaring.so for --mode module server launches")
    parser.add_argument("--module-sha",
                        help="Resolved redis-roaring commit SHA to record in JSON/Markdown metadata")
    parser.add_argument("--module-command-prefix", choices=("R", "R64"), default="R",
                        help="redis-roaring command family to use for module mode")
    parser.add_argument("--module-seed-chunk-size", type=int, default=4096,
                        help="Integer count per redis-roaring SETINTARRAY/APPENDINTARRAY seeding command")
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
    parser.add_argument("--benchmark-profile", choices=("full", "smoke", "small-sets", "bitsets", "mixed-bitop"),
                        default="full",
                        help="Workload profile to run when --only is not supplied")
    parser.add_argument("--report-view", choices=("performance", "memory", "payload", "combined"),
                        default="combined",
                        help="Compare Markdown sections to publish")
    parser.add_argument("--skip-persistence", action="store_true",
                        help="Skip DUMP/RESTORE, RDB save/load, and AOF rewrite phases")
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--request-scale", type=float, default=1.0,
                        help="Scale factor applied to command workload request counts")
    parser.add_argument("--min-requests", type=int, default=1000,
                        help="Minimum measured redis-benchmark requests for scaled multi-request workloads")
    parser.add_argument("--one-shot-min-runs", type=int, default=5,
                        help="Minimum samples for one-shot latency workloads")
    parser.add_argument("--warmup", action="store_true", default=True,
                        help="Run workload warmups where defined (default: enabled)")
    parser.add_argument("--no-warmup", dest="warmup", action="store_false")
    parser.add_argument("--json-out", help="Optional path for machine-readable results")
    parser.add_argument("--csv-out", help="Optional path for CSV result rows")
    parser.add_argument("--markdown-out", help="Optional path for Markdown summary")
    parser.add_argument("--runs", type=int, default=1,
                        help="Repeat command workloads and report medians")
    parser.add_argument("--ping-canary", action="store_true",
                        help="Measure max PING latency/gap during one-shot and persistence phases")
    parser.add_argument("--ping-canary-interval", type=float, default=0.01,
                        help="Seconds between canary PING attempts")
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
    parser.add_argument("--croaring-realdata-dir",
                        help="Directory containing CRoaring/real-roaring integer-set files or zip archives")
    parser.add_argument("--download-croaring-realdata", action="store_true",
                        help="Download selected real-roaring dataset archives into --croaring-realdata-dir")
    parser.add_argument("--realdata-archives",
                        help="Comma-separated real-roaring archive basenames to download")
    parser.add_argument("--realdata-max-files", type=int, default=8,
                        help="Maximum realdata files to load")
    parser.add_argument("--realdata-max-values", type=int, default=250_000,
                        help="Maximum set bits to load from each realdata file")
    parser.add_argument("--compare-before-src-dir",
                        help="Run a legacy-mode baseline from this src directory")
    parser.add_argument("--compare-after-src-dir",
                        help="Run a native-mode candidate from this src directory")
    parser.add_argument("--compare-legacy-src-dir",
                        help="Optional PR legacy guardrail src directory")
    parser.add_argument("--compare-module-src-dir",
                        help="Optional Redis host src directory for redis-roaring module compare mode")
    parser.add_argument("--compare-module-path",
                        help="Optional libredis-roaring.so path for redis-roaring module compare mode")
    parser.add_argument("--compare-out",
                        help="Write combined compare JSON and Markdown using this path prefix")
    parser.add_argument("--socket-timeout", type=float, default=30.0)
    parser.add_argument("--benchmark-timeout", type=float, default=120.0)
    parser.add_argument("--persistence-timeout", type=float, default=120.0)
    return parser.parse_args()


def run_compare(args: argparse.Namespace) -> int:
    if not args.compare_before_src_dir or not args.compare_after_src_dir:
        raise BenchError("--compare-before-src-dir and --compare-after-src-dir are required for compare mode")
    prefix = Path(args.compare_out or "bitmap-bench-compare")
    if prefix.suffix:
        json_path = prefix
        markdown_path = prefix.with_suffix(".md")
        csv_path = prefix.with_suffix(".csv")
    else:
        json_path = prefix.with_suffix(".json")
        markdown_path = prefix.with_suffix(".md")
        csv_path = prefix.with_suffix(".csv")
    json_path.parent.mkdir(parents=True, exist_ok=True)

    runs: list[dict[str, Any]] = [
        {
            "label": "redis_before",
            "mode": "legacy",
            "src_dir": args.compare_before_src_dir,
            "port": args.port,
            "module_path": None,
        },
        {
            "label": "redis_pr_native",
            "mode": "native",
            "src_dir": args.compare_after_src_dir,
            "port": args.port + 1,
            "module_path": None,
        },
    ]
    if args.compare_module_src_dir or args.compare_module_path:
        if not args.compare_module_src_dir or not args.compare_module_path:
            raise BenchError("--compare-module-src-dir and --compare-module-path must be provided together")
        runs.append({
            "label": MODULE_RESULT_LABEL,
            "mode": "module",
            "src_dir": args.compare_module_src_dir,
            "port": args.port + 2,
            "module_path": args.compare_module_path,
        })
    if args.compare_legacy_src_dir:
        runs.append({
            "label": "redis_pr_legacy",
            "mode": "legacy",
            "src_dir": args.compare_legacy_src_dir,
            "port": args.port + 2 + (1 if args.compare_module_src_dir else 0),
            "module_path": None,
        })

    payloads = []
    with tempfile.TemporaryDirectory(prefix="bitmap-bench-compare-") as tmp:
        for run in runs:
            label = run["label"]
            mode = run["mode"]
            print(f"\n=== compare run: {label} ({mode}) ===", file=sys.stderr)
            run_args = argparse.Namespace(**vars(args))
            run_args.src_dir = run["src_dir"]
            run_args.mode = mode
            run_args.mode_label = label
            run_args.port = run["port"]
            run_args.module_path = run["module_path"]
            run_args.compare_before_src_dir = None
            run_args.compare_after_src_dir = None
            run_args.compare_legacy_src_dir = None
            run_args.compare_module_src_dir = None
            run_args.compare_module_path = None
            run_args.compare_out = None
            run_args.json_out = str(Path(tmp) / f"{label}.json")
            run_args.csv_out = None
            run_args.markdown_out = None
            bench = RedisBitmapBench(run_args)
            code = bench.run()
            if code != 0:
                return code
            payloads.append(json.loads(Path(run_args.json_out).read_text(encoding="utf-8")))

    comparison = compare_payloads(payloads)
    combined = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "benchmark_profile": args.benchmark_profile,
        "report_view": args.report_view,
        "labels": [payload.get("mode_label", payload.get("mode", "run")) for payload in payloads],
        "runs": payloads,
        "comparison": comparison,
    }
    json_path.write_text(json.dumps(combined, indent=2), encoding="utf-8")
    markdown_path.write_text("\n".join(compare_markdown_lines(comparison, payloads, args.report_view)) + "\n",
                             encoding="utf-8")
    write_compare_csv(csv_path, comparison)
    print(f"compare json written to {json_path}")
    print(f"compare markdown written to {markdown_path}")
    print(f"compare csv written to {csv_path}")
    return 0


def compare_payloads(payloads: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_name: dict[str, dict[str, dict[str, Any]]] = {}
    all_labels = [payload.get("mode_label", payload.get("mode", "run")) for payload in payloads]
    for payload in payloads:
        label = payload.get("mode_label", payload.get("mode", "run"))
        for result in payload.get("results", []) + payload.get("persistence", []):
            by_name.setdefault(result["name"], {})[label] = result

    rows = []
    for name in sorted(by_name):
        row: dict[str, Any] = {"workload": name}
        first = next(iter(by_name[name].values()))
        row["category"] = first.get("category", "")
        row["story"] = first.get("extra", {}).get("story", row["category"])
        row["dataset"] = first.get("extra", {}).get("dataset", "-")
        row["target_groups"] = ",".join(first.get("extra", {}).get("target_groups", [])) or "-"
        row["metric"] = "time_per_op_us" if first.get("time_per_op_us") is not None else "elapsed_ms"
        row["metric_direction"] = "lower_is_better"
        for label in all_labels:
            result = by_name[name].get(label)
            row[label] = compare_result_value(result) if result else missing_compare_value(label)
            row[f"{label}_time_per_op_mean_us"] = compare_result_time_stat(result, "mean") if result else missing_compare_value(label)
            row[f"{label}_time_per_op_median_us"] = compare_result_time_stat(result, "median") if result else missing_compare_value(label)
            row[f"{label}_time_per_op_min_us"] = compare_result_time_stat(result, "min") if result else missing_compare_value(label)
            row[f"{label}_time_per_op_max_us"] = compare_result_time_stat(result, "max") if result else missing_compare_value(label)
            row[f"{label}_time_per_op_stdev_us"] = compare_result_stdev(result) if result else missing_compare_value(label)
            row[f"{label}_time_per_op_cv_percent"] = compare_result_time_stat(result, "cv_percent") if result else missing_compare_value(label)
            row[f"{label}_qps"] = compare_result_qps(result) if result else missing_compare_value(label)
            row[f"{label}_memory_bytes"] = result.get("memory_usage_bytes") if result else missing_compare_value(label)
            row[f"{label}_payload_bytes"] = result.get("payload_size_bytes") if result else missing_compare_value(label)
        row["core_vs_string_percent"] = metric_delta_percent(
            row.get("redis_pr_native"),
            row.get("redis_before"),
            row["metric_direction"],
        )
        row["native_delta_percent"] = row["core_vs_string_percent"]
        row["core_vs_module_percent"] = metric_delta_percent(
            row.get("redis_pr_native"),
            row.get(MODULE_RESULT_LABEL),
            row["metric_direction"],
        )
        row["memory_core_vs_string_percent"] = metric_delta_percent(
            row.get("redis_pr_native_memory_bytes"),
            row.get("redis_before_memory_bytes"),
            "lower_is_better",
        )
        row["memory_core_vs_module_percent"] = metric_delta_percent(
            row.get("redis_pr_native_memory_bytes"),
            row.get(f"{MODULE_RESULT_LABEL}_memory_bytes"),
            "lower_is_better",
        )
        row["payload_core_vs_string_percent"] = metric_delta_percent(
            row.get("redis_pr_native_payload_bytes"),
            row.get("redis_before_payload_bytes"),
            "lower_is_better",
        )
        row["payload_core_vs_module_percent"] = metric_delta_percent(
            row.get("redis_pr_native_payload_bytes"),
            row.get(f"{MODULE_RESULT_LABEL}_payload_bytes"),
            "lower_is_better",
        )
        notes = []
        if row.get(MODULE_RESULT_LABEL) == "N/A":
            notes.append("No redis-roaring equivalent")
        if row["category"] == "persistence":
            notes.append("Payload formats are implementation-specific")
        row["notes"] = "; ".join(notes) if notes else "-"
        rows.append(row)
    return rows


def compare_markdown_lines(rows: list[dict[str, Any]], payloads: list[dict[str, Any]],
                           report_view: str = "combined") -> list[str]:
    lines = [
        "# Redis Native Bitmap Benchmark Compare",
        "",
        "| Run | Mode | Module | Runner |",
        "| --- | --- | --- | --- |",
    ]
    markdown_payloads = [
        payload for payload in payloads
        if payload.get("mode_label", payload.get("mode", "run")) in COMPARE_LABELS
    ]
    for payload in markdown_payloads:
        env = payload.get("environment", {})
        run_link = markdown_commit_link(
            payload.get("mode_label", "-"),
            env.get("source_repo_url", ""),
            env.get("source_sha", ""),
        )
        module_link = "-"
        if env.get("module_sha"):
            module_link = markdown_commit_link(
                "redis-roaring",
                env.get("module_repo_url", ""),
                env.get("module_sha", ""),
            )
        lines.append(
            f"| {run_link} | {payload.get('mode', '-')} | "
            f"{module_link} | "
            f"{markdown_runner_link(env)} |"
        )

    lines.extend(["", "Delta columns are positive when Redis core Roaring is better."])

    if report_view in ("performance", "combined"):
        append_performance_compare(lines, rows)
    if report_view in ("memory", "combined"):
        append_resource_compare(
            lines,
            rows,
            title="Memory Usage",
            field_suffix="memory_bytes",
            string_delta="memory_core_vs_string_percent",
            module_delta="memory_core_vs_module_percent",
        )
    if report_view in ("payload", "combined"):
        append_resource_compare(
            lines,
            rows,
            title="Storage",
            description="Storage is the serialized byte size recorded by Redis DUMP/RDB/AOF-style payload workloads; lower is better.",
            field_suffix="payload_bytes",
            string_delta="payload_core_vs_string_percent",
            module_delta="payload_core_vs_module_percent",
        )
    return lines


def append_performance_compare(lines: list[str], rows: list[dict[str, Any]]) -> None:
    lines.extend([
        "",
        "## Performance",
        "",
        "| Operation | Redis string | Redis core Roaring | redis-roaring module | Core delta |",
        "| --- | ---: | ---: | ---: | --- |",
    ])
    for row in rows:
        lines.append(
            f"| {compare_operation_label(row)} | "
            f"{fmt_performance_cell(row, 'redis_before')} | "
            f"{fmt_performance_cell(row, 'redis_pr_native')} | "
            f"{fmt_performance_cell(row, MODULE_RESULT_LABEL)} | "
            f"{fmt_delta_cell(row, 'core_vs_string_percent', 'core_vs_module_percent')} |"
        )


def append_resource_compare(lines: list[str], rows: list[dict[str, Any]], title: str,
                            field_suffix: str, string_delta: str, module_delta: str,
                            description: str = "") -> None:
    resource_rows = [
        row for row in rows
        if any(isinstance(row.get(f"{label}_{field_suffix}"), (int, float)) for label in COMPARE_LABELS)
    ]
    lines.extend([
        "",
        f"## {title}",
        "",
    ])
    if description:
        lines.extend([description, ""])
    lines.extend([
        "| Operation | Redis string | Redis core Roaring | redis-roaring module | Core delta |",
        "| --- | ---: | ---: | ---: | --- |",
    ])
    for row in resource_rows:
        lines.append(
            f"| {compare_operation_label(row)} | "
            f"{fmt_any(row.get(f'redis_before_{field_suffix}'))} | "
            f"{fmt_any(row.get(f'redis_pr_native_{field_suffix}'))} | "
            f"{fmt_any(row.get(f'{MODULE_RESULT_LABEL}_{field_suffix}'))} | "
            f"{fmt_delta_cell(row, string_delta, module_delta)} |"
        )


def compare_operation_label(row: dict[str, Any]) -> str:
    label = markdown_escape(row.get("workload", "-"))
    details = []
    dataset = row.get("dataset", "-")
    groups = row.get("target_groups", "-")
    story = row.get("story", "-")
    if dataset and dataset != "-":
        details.append(f"dataset: {markdown_escape(dataset)}")
    if groups and groups != "-":
        details.append(f"groups: {markdown_escape(groups)}")
    if story and story not in ("-", row.get("category", "-")):
        details.append(f"story: {markdown_escape(story)}")
    if details:
        label += f"<br><sub>{' / '.join(details)}</sub>"
    return label


def fmt_performance_cell(row: dict[str, Any], label: str) -> str:
    value = row.get(label)
    if not isinstance(value, (int, float)):
        return fmt_any(value)
    unit = "ms" if row.get("metric") == "elapsed_ms" else "us"
    stdev = row.get(f"{label}_time_per_op_stdev_us")
    if unit == "us" and isinstance(stdev, (int, float)):
        rounded_value, rounded_stdev = fmt_value_with_uncertainty(value, stdev)
        return f"{rounded_value} +/- {rounded_stdev} {unit}"
    return f"{fmt_any(value)} {unit}"


def fmt_value_with_uncertainty(value: float, uncertainty: float) -> tuple[str, str]:
    if uncertainty == 0:
        return fmt_any(value), "0"
    if not math.isfinite(uncertainty):
        return fmt_any(value), fmt_any(uncertainty)
    decimals = uncertainty_decimal_places(abs(uncertainty), significant_digits=2)
    return format_rounded_number(value, decimals), format_rounded_number(uncertainty, decimals)


def uncertainty_decimal_places(uncertainty: float, significant_digits: int) -> int:
    magnitude = math.floor(math.log10(uncertainty))
    return significant_digits - 1 - magnitude


def format_rounded_number(value: float, decimals: int) -> str:
    rounded = round(value, decimals)
    if decimals > 0:
        return f"{rounded:.{decimals}f}"
    return f"{rounded:.0f}"


def fmt_delta_cell(row: dict[str, Any], string_delta: str, module_delta: str) -> str:
    parts = []
    if row.get(string_delta) is not None:
        parts.append(f"{fmt_delta(row.get(string_delta))}% vs string")
    if row.get(module_delta) is not None:
        parts.append(f"{fmt_delta(row.get(module_delta))}% vs module")
    return "<br>".join(parts) if parts else "-"


def write_compare_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fields = [
        "story",
        "dataset",
        "target_groups",
        "workload",
        "category",
        "metric",
        "metric_direction",
        "redis_before",
        "redis_before_time_per_op_mean_us",
        "redis_before_time_per_op_median_us",
        "redis_before_time_per_op_min_us",
        "redis_before_time_per_op_max_us",
        "redis_before_time_per_op_stdev_us",
        "redis_before_time_per_op_cv_percent",
        "redis_before_qps",
        "redis_before_memory_bytes",
        "redis_before_payload_bytes",
        "redis_pr_native",
        "redis_pr_native_time_per_op_mean_us",
        "redis_pr_native_time_per_op_median_us",
        "redis_pr_native_time_per_op_min_us",
        "redis_pr_native_time_per_op_max_us",
        "redis_pr_native_time_per_op_stdev_us",
        "redis_pr_native_time_per_op_cv_percent",
        "redis_pr_native_qps",
        "redis_pr_native_memory_bytes",
        "redis_pr_native_payload_bytes",
        MODULE_RESULT_LABEL,
        f"{MODULE_RESULT_LABEL}_time_per_op_mean_us",
        f"{MODULE_RESULT_LABEL}_time_per_op_median_us",
        f"{MODULE_RESULT_LABEL}_time_per_op_min_us",
        f"{MODULE_RESULT_LABEL}_time_per_op_max_us",
        f"{MODULE_RESULT_LABEL}_time_per_op_stdev_us",
        f"{MODULE_RESULT_LABEL}_time_per_op_cv_percent",
        f"{MODULE_RESULT_LABEL}_qps",
        f"{MODULE_RESULT_LABEL}_memory_bytes",
        f"{MODULE_RESULT_LABEL}_payload_bytes",
        "core_vs_string_percent",
        "core_vs_module_percent",
        "memory_core_vs_string_percent",
        "memory_core_vs_module_percent",
        "payload_core_vs_string_percent",
        "payload_core_vs_module_percent",
        "native_delta_percent",
        "redis_pr_legacy",
        "redis_pr_legacy_time_per_op_mean_us",
        "redis_pr_legacy_time_per_op_median_us",
        "redis_pr_legacy_time_per_op_min_us",
        "redis_pr_legacy_time_per_op_max_us",
        "redis_pr_legacy_time_per_op_stdev_us",
        "redis_pr_legacy_time_per_op_cv_percent",
        "redis_pr_legacy_qps",
        "redis_pr_legacy_memory_bytes",
        "redis_pr_legacy_payload_bytes",
        "notes",
    ]
    with open(path, "w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def compare_result_value(result: Optional[dict[str, Any]]) -> Any:
    if not result:
        return None
    value = result.get("time_per_op_us")
    if value is not None:
        return value
    return result.get("elapsed_ms")


def compare_result_stdev(result: Optional[dict[str, Any]]) -> Any:
    return compare_result_time_stat(result, "stdev")


def compare_result_time_stat(result: Optional[dict[str, Any]], name: str) -> Any:
    if not result:
        return None
    value = result.get("extra", {}).get(f"time_per_op_us_{name}")
    if value is None and name in ("mean", "median", "min", "max"):
        return result.get("time_per_op_us")
    return value


def compare_result_qps(result: Optional[dict[str, Any]]) -> Any:
    if not result:
        return None
    return result.get("qps") if result.get("qps") is not None else "N/A"


def missing_compare_value(label: str) -> Any:
    return "N/A" if label == MODULE_RESULT_LABEL else None


def metric_delta_percent(candidate: Any, baseline: Any, direction: str) -> Optional[float]:
    if not isinstance(candidate, (int, float)) or not isinstance(baseline, (int, float)):
        return None
    if baseline == 0:
        return None
    if direction == "lower_is_better":
        return (baseline - candidate) / baseline * 100.0
    return (candidate - baseline) / baseline * 100.0


def fmt_any(value: Any) -> str:
    if value is None or value == "":
        return "-"
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


def fmt_delta(value: Any) -> str:
    if value is None or value == "":
        return "-"
    if isinstance(value, float):
        return f"{value:+.0f}"
    return str(value)


def main() -> int:
    args = parse_args()
    try:
        if args.compare_before_src_dir or args.compare_after_src_dir:
            return run_compare(args)
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
