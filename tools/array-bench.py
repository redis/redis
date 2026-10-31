#!/usr/bin/env python3
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Optional


QPS_RE = re.compile(r"([0-9]+(?:\.[0-9]+)?)\s+requests per second")


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


@dataclass
class Result:
    name: str
    description: str
    qps: float
    requests: int
    clients: int
    pipeline: int
    rand_range: int
    command: list[str]
    raw_output: str


class BenchError(RuntimeError):
    pass


class RedisArrayBench:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.base_dir = Path(__file__).resolve().parent
        repo_root = self.base_dir.parent
        src_dir = Path(args.src_dir) if args.src_dir else repo_root / "src"
        self.redis_server = str(src_dir / "redis-server")
        self.redis_cli = str(src_dir / "redis-cli")
        self.redis_benchmark = str(src_dir / "redis-benchmark")
        self.server_proc: Optional[subprocess.Popen[str]] = None
        self.server_dir: Optional[tempfile.TemporaryDirectory[str]] = None
        self.host = args.host
        self.port = args.port
        self.db = args.db
        self.results: list[Result] = []

        for binary in (self.redis_server, self.redis_cli, self.redis_benchmark):
            if not os.path.exists(binary):
                raise BenchError(f"missing binary: {binary}")

    def run(self) -> int:
        try:
            if self.args.start_server:
                self.start_server()
            self.prepare_data()
            self.print_dataset_summary()
            for workload in self.selected_workloads():
                result = self.run_workload(workload)
                self.results.append(result)
                print(f"{result.name:28s} {result.qps:12.2f} req/s")
            self.print_summary()
            if self.args.json_out:
                with open(self.args.json_out, "w", encoding="utf-8") as fp:
                    json.dump({
                        "host": self.host,
                        "port": self.port,
                        "db": self.db,
                        "results": [asdict(r) for r in self.results],
                    }, fp, indent=2)
                print(f"json written to {self.args.json_out}")
            return 0
        finally:
            if self.args.start_server and not self.args.keep_server:
                self.stop_server()

    def start_server(self) -> None:
        self.server_dir = tempfile.TemporaryDirectory(prefix="array-bench-")
        cmd = [
            self.redis_server,
            "--port", str(self.port),
            "--save", "",
            "--appendonly", "no",
            "--dir", self.server_dir.name,
            "--loglevel", "warning",
            "--daemonize", "no",
        ]
        self.server_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.wait_for_ping(timeout=10.0)

    def stop_server(self) -> None:
        if self.server_proc is not None and self.server_proc.poll() is None:
            self.server_proc.send_signal(signal.SIGTERM)
            try:
                self.server_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server_proc.kill()
                self.server_proc.wait(timeout=5)
        if self.server_dir is not None:
            self.server_dir.cleanup()
        self.server_proc = None
        self.server_dir = None

    def wait_for_ping(self, timeout: float) -> None:
        deadline = time.time() + timeout
        last_error = None
        while time.time() < deadline:
            if self.server_proc is not None and self.server_proc.poll() is not None:
                raise BenchError(
                    "server exited before becoming ready:\n"
                    f"{self.read_server_output().strip()}"
                )
            try:
                cmd = [
                    self.redis_cli,
                    "-h", self.host,
                    "-p", str(self.port),
                    "-n", str(self.db),
                    "--raw",
                    "PING",
                ]
                probe = subprocess.run(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                if probe.returncode != 0:
                    raise BenchError(probe.stderr.strip() or probe.stdout.strip())
                out = probe.stdout.strip()
                if out == "PONG":
                    return
            except Exception as exc:  # pragma: no cover - startup race handling
                last_error = exc
            time.sleep(0.05)
        raise BenchError(
            f"server did not start on {self.host}:{self.port}: {last_error}\n"
            f"{self.read_server_output().strip()}"
        )

    def read_server_output(self) -> str:
        if self.server_proc is None or self.server_proc.stdout is None:
            return ""
        try:
            return self.server_proc.stdout.read()
        except Exception:  # pragma: no cover - best effort diagnostics
            return ""

    def cli(self, command: list[str], raw: bool = False) -> str:
        cmd = [self.redis_cli, "-h", self.host, "-p", str(self.port), "-n", str(self.db)]
        if raw:
            cmd.append("--raw")
        cmd.extend(command)
        return subprocess.check_output(cmd, text=True)

    def pipe(self, payload: bytes) -> None:
        cmd = [self.redis_cli, "-h", self.host, "-p", str(self.port), "-n", str(self.db), "--pipe"]
        proc = subprocess.run(cmd, input=payload, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode != 0:
            raise BenchError(f"redis-cli --pipe failed:\n{proc.stdout.decode('utf-8', 'replace')}")
        out = proc.stdout.decode("utf-8", "replace")
        if "errors: 0, replies:" not in out:
            raise BenchError(f"unexpected --pipe output:\n{out}")

    @staticmethod
    def resp(parts: list[str]) -> bytes:
        out = [f"*{len(parts)}\r\n".encode()]
        for part in parts:
            data = part.encode("utf-8")
            out.append(f"${len(data)}\r\n".encode())
            out.append(data)
            out.append(b"\r\n")
        return b"".join(out)

    def prepare_data(self) -> None:
        print("preparing datasets...", file=sys.stderr)
        self.cli(["FLUSHDB"])
        payload = bytearray()
        payload += self.resp(["DEL", "bench:array:dense:num", "bench:array:dense:text", "bench:array:sparse:text", "bench:array:append", "bench:array:ring"])
        payload += self.build_dense_numeric()
        payload += self.build_dense_text()
        payload += self.build_sparse_text()
        self.pipe(bytes(payload))

    def build_dense_numeric(self) -> bytes:
        key = "bench:array:dense:num"
        total = self.args.dense_len
        batch = 256
        payload = bytearray()
        for start in range(0, total, batch):
            values = [str(start + i) for i in range(min(batch, total - start))]
            payload += self.resp(["ARSET", key, str(start), *values])
        return bytes(payload)

    def build_dense_text(self) -> bytes:
        key = "bench:array:dense:text"
        total = self.args.dense_len
        batch = 128
        payload = bytearray()
        for start in range(0, total, batch):
            values = []
            for i in range(start, min(start + batch, total)):
                mod = i % 4
                if mod == 0:
                    values.append(f"row:{i} alpha encoding complexity")
                elif mod == 1:
                    values.append(f"row:{i} beta sparse vector")
                elif mod == 2:
                    values.append(f"row:{i} gamma dense matcher")
                else:
                    values.append(f"row:{i} delta encoding helper")
            payload += self.resp(["ARSET", key, str(start), *values])
        return bytes(payload)

    def build_sparse_text(self) -> bytes:
        key = "bench:array:sparse:text"
        clusters = [
            (0, 97, 384),
            (8_388_608, 113, 640),
            (16_777_216, 127, 896),
            (25_165_824, 151, 896),
        ]
        batch_pairs = 64
        pairs: list[str] = []
        payload = bytearray()
        nth = 0
        for base, stride, count in clusters:
            for i in range(count):
                idx = base + i * stride
                mod = nth % 4
                if mod == 0:
                    value = f"slot:{idx} alpha encoding complexity"
                elif mod == 1:
                    value = f"slot:{idx} beta sparse needle"
                elif mod == 2:
                    value = f"slot:{idx} gamma dense helper"
                else:
                    value = f"slot:{idx} delta complexity marker"
                pairs.extend([str(idx), value])
                nth += 1
                if len(pairs) >= batch_pairs * 2:
                    payload += self.resp(["ARMSET", key, *pairs])
                    pairs.clear()
        if pairs:
            payload += self.resp(["ARMSET", key, *pairs])
        return bytes(payload)

    def print_dataset_summary(self) -> None:
        summary = {
            "bench:array:dense:num": {
                "count": self.cli(["ARCOUNT", "bench:array:dense:num"], raw=True).strip(),
                "len": self.cli(["ARLEN", "bench:array:dense:num"], raw=True).strip(),
            },
            "bench:array:dense:text": {
                "count": self.cli(["ARCOUNT", "bench:array:dense:text"], raw=True).strip(),
                "len": self.cli(["ARLEN", "bench:array:dense:text"], raw=True).strip(),
            },
            "bench:array:sparse:text": {
                "count": self.cli(["ARCOUNT", "bench:array:sparse:text"], raw=True).strip(),
                "len": self.cli(["ARLEN", "bench:array:sparse:text"], raw=True).strip(),
            },
        }
        print("dataset:")
        for key, info in summary.items():
            print(f"  {key}: count={info['count']} len={info['len']}")

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
        dense_range_end = min(8192 + 31, self.args.dense_len - 1)
        return [
            Workload("arget_dense_rand", "ARGET dense random hit", ["ARGET", "bench:array:dense:num", "__rand_int__"], 200_000, 50, 16, rand_range=self.args.dense_len),
            Workload("armget_dense_4_rand", "ARMGET dense 4 random hits", ["ARMGET", "bench:array:dense:num", "__rand_int__", "__rand_int__", "__rand_int__", "__rand_int__"], 100_000, 50, 16, rand_range=self.args.dense_len),
            Workload("argetrange_dense_32", "ARGETRANGE dense 32 hot", ["ARGETRANGE", "bench:array:dense:num", "8192", str(dense_range_end)], 50_000, 32, 8),
            Workload("arscan_dense_limit_100", "ARSCAN dense LIMIT 100", ["ARSCAN", "bench:array:dense:text", "0", str(self.args.dense_len - 1), "LIMIT", "100"], 50_000, 24, 4),
            Workload("argrep_match_dense", "ARGREP MATCH dense", ["ARGREP", "bench:array:dense:text", "0", str(self.args.dense_len - 1), "MATCH", "encoding", "LIMIT", "20", "WITHVALUES"], 20_000, 20, 2),
            Workload("argrep_re_dense_nocase", "ARGREP RE dense nocase", ["ARGREP", "bench:array:dense:text", "0", str(self.args.dense_len - 1), "RE", "encoding|complexity|helper", "NOCASE", "LIMIT", "20", "WITHVALUES"], 20_000, 20, 2),
            Workload("arop_sum_dense_4096", "AROP SUM dense 4096", ["AROP", "bench:array:dense:num", "0", "4095", "SUM"], 50_000, 24, 4),
            Workload("arget_sparse_rand", "ARGET sparse random mostly miss", ["ARGET", "bench:array:sparse:text", "__rand_int__"], 200_000, 50, 16, rand_range=self.args.sparse_space),
            Workload("arscan_sparse_limit_100", "ARSCAN sparse LIMIT 100", ["ARSCAN", "bench:array:sparse:text", "0", str(self.args.sparse_space - 1), "LIMIT", "100"], 25_000, 20, 2),
            Workload("argrep_match_sparse", "ARGREP MATCH sparse", ["ARGREP", "bench:array:sparse:text", "0", str(self.args.sparse_space - 1), "MATCH", "encoding", "LIMIT", "20", "WITHVALUES"], 10_000, 16, 1),
            Workload("arop_used_sparse", "AROP USED sparse", ["AROP", "bench:array:sparse:text", "0", str(self.args.sparse_space - 1), "USED"], 25_000, 20, 2),
            Workload("arset_dense_rand", "ARSET dense random update", ["ARSET", "bench:array:dense:num", "__rand_int__", "42"], 150_000, 50, 16, rand_range=self.args.dense_len),
            Workload("armset_dense_4_rand", "ARMSET dense 4 random updates", ["ARMSET", "bench:array:dense:num", "__rand_int__", "11", "__rand_int__", "22", "__rand_int__", "33", "__rand_int__", "44"], 100_000, 50, 16, rand_range=self.args.dense_len),
            Workload("arinsert_append_hot", "ARINSERT append hot path", ["ARINSERT", "bench:array:append", "x"], 50_000, 24, 8, setup="reset_append"),
            Workload("arring_hot_1024", "ARRING size 1024 hot path", ["ARRING", "bench:array:ring", "1024", "x"], 100_000, 50, 16, setup="reset_ring"),
        ]

    def run_workload(self, workload: Workload) -> Result:
        if workload.setup:
            getattr(self, workload.setup)()
        if self.args.warmup and workload.warmup_requests > 0:
            self.invoke_benchmark(workload, workload.warmup_requests, quiet=True)
        raw = self.invoke_benchmark(workload, self.scale_requests(workload.requests), quiet=True)
        qps = self.parse_qps(raw)
        return Result(
            name=workload.name,
            description=workload.description,
            qps=qps,
            requests=self.scale_requests(workload.requests),
            clients=workload.clients,
            pipeline=workload.pipeline,
            rand_range=workload.rand_range,
            command=workload.command,
            raw_output=raw.strip(),
        )

    def invoke_benchmark(self, workload: Workload, requests: int, quiet: bool) -> str:
        cmd = [
            self.redis_benchmark,
            "-h", self.host,
            "-p", str(self.port),
            "--dbnum", str(self.db),
            "-n", str(requests),
            "-c", str(workload.clients),
            "-P", str(workload.pipeline),
            "--seed", str(self.args.seed),
        ]
        if quiet:
            cmd.append("-q")
        if workload.rand_range:
            cmd.extend(["-r", str(workload.rand_range)])
        cmd.extend(workload.command)
        return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)

    def parse_qps(self, raw: str) -> float:
        m = QPS_RE.search(raw)
        if not m:
            raise BenchError(f"could not parse qps from redis-benchmark output:\n{raw}")
        return float(m.group(1))

    def scale_requests(self, requests: int) -> int:
        scaled = int(requests * self.args.request_scale)
        return max(1000, scaled)

    def reset_append(self) -> None:
        self.cli(["DEL", "bench:array:append"])

    def reset_ring(self) -> None:
        self.cli(["DEL", "bench:array:ring"])

    def print_summary(self) -> None:
        print("\nsummary:")
        print("| workload | qps | req | c | P | notes |")
        print("|---|---:|---:|---:|---:|---|")
        for r in self.results:
            notes = r.description
            if r.rand_range:
                notes += f", rand=0..{r.rand_range - 1}"
            print(f"| {r.name} | {r.qps:.2f} | {r.requests} | {r.clients} | {r.pipeline} | {notes} |")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Standalone Array benchmark harness. It uses DB 9 by default, "
            "flushes that DB, loads deterministic Array datasets, and runs "
            "custom redis-benchmark workloads."
        )
    )
    parser.add_argument("--src-dir", help="Path to the src directory containing redis-server, redis-cli, and redis-benchmark")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6395)
    parser.add_argument("--db", type=int, default=9)
    parser.add_argument("--start-server", action="store_true", default=True,
                        help="Start an ephemeral redis-server on --port (default: enabled)")
    parser.add_argument("--no-start-server", dest="start_server", action="store_false",
                        help="Use an already running server instead of starting one")
    parser.add_argument("--keep-server", action="store_true",
                        help="Do not stop the ephemeral server after the run")
    parser.add_argument("--only", help="Comma-separated workload names to run")
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--request-scale", type=float, default=1.0,
                        help="Scale factor applied to all workload request counts")
    parser.add_argument("--warmup", action="store_true", default=True,
                        help="Run a short warmup before each benchmark (default: enabled)")
    parser.add_argument("--no-warmup", dest="warmup", action="store_false")
    parser.add_argument("--json-out", help="Optional path for machine-readable results")
    parser.add_argument("--dense-len", type=int, default=16_384,
                        help="Number of contiguous dense elements to preload")
    parser.add_argument("--sparse-space", type=int, default=30_000_000,
                        help="Logical range used by sparse benchmarks")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        bench = RedisArrayBench(args)
        return bench.run()
    except BenchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        output = exc.output if isinstance(exc.output, str) else exc.output.decode("utf-8", "replace")
        print(output, file=sys.stderr)
        return exc.returncode or 1


if __name__ == "__main__":
    raise SystemExit(main())
