#!/usr/bin/env python3
"""Reproducible native/module integer-array benchmarks; Python stdlib only."""

import argparse
import contextlib
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import socket
import statistics
import subprocess
import tempfile
import time


def encode(parts):
    parts = [part if isinstance(part, bytes) else str(part).encode() for part in parts]
    return b"*%d\r\n" % len(parts) + b"".join(
        b"$%d\r\n" % len(part) + part + b"\r\n" for part in parts)


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(60)
        self.sock.connect(str(path))
        self.reader = self.sock.makefile("rb")

    def close(self):
        self.reader.close()
        self.sock.close()

    def read(self):
        line = self.reader.readline()
        if not line.endswith(b"\r\n"):
            raise RuntimeError("incomplete RESP reply")
        kind, value = line[:1], line[1:-2]
        if kind == b"-":
            raise RuntimeError(value.decode())
        if kind == b"+":
            return value
        if kind == b":":
            return int(value)
        if kind == b"$":
            size = int(value)
            if size == -1:
                return None
            data = self.reader.read(size + 2)
            if len(data) != size + 2 or data[-2:] != b"\r\n":
                raise RuntimeError("incomplete RESP bulk string")
            return data[:-2]
        if kind == b"*":
            return [self.read() for _ in range(int(value))]
        raise RuntimeError(f"unexpected RESP type: {kind!r}")

    def call(self, *parts):
        self.sock.sendall(encode(parts))
        return self.read()

    def pipeline(self, commands):
        self.sock.sendall(b"".join(encode(command) for command in commands))
        for _ in commands:
            self.read()


@contextlib.contextmanager
def server(src, module=None):
    with tempfile.TemporaryDirectory(prefix="intarray-bench-") as directory:
        directory = Path(directory)
        path = directory / "redis.sock"
        command = [str(src / "redis-server"), "--port", "0", "--unixsocket", str(path),
                   "--unixsocketperm", "700", "--save", "", "--appendonly", "no",
                   "--dir", str(directory), "--logfile", str(directory / "server.log")]
        if module:
            command += ["--loadmodule", str(module)]
        else:
            command += ["--bitmap-default-roaring", "yes",
                        "--proto-max-bulk-len", str(1 << 48)]
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        client = None
        try:
            for _ in range(200):
                if process.poll() is not None:
                    raise RuntimeError((directory / "server.log").read_text())
                if path.exists():
                    try:
                        client = Client(path)
                        if client.call("PING") == b"PONG":
                            break
                    except (OSError, RuntimeError):
                        if client:
                            client.close()
                            client = None
                time.sleep(0.05)
            else:
                raise RuntimeError("benchmark server did not start")
            yield client, path
        finally:
            if client:
                client.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def datasets():
    yield "empty", []
    for count in (16, 4096, 65536):
        yield f"dense-{count}", list(range(count))
        yield f"sparse-{count}", [index * 65537 for index in range(count)]
    yield "bitset-65536", [index * 2 for index in range(65536)]
    yield "high64-4096", [(1 << 40) + index * 65537 for index in range(4096)]


def seed(client, backend, key, values):
    if not values:
        return
    if backend == "native":
        for offset in range(0, len(values), 512):
            client.pipeline([["SETBIT", key, value, 1] for value in values[offset:offset + 512]])
        # Match the module's explicit run optimization and shrink step using
        # the native bitmap's normal persistence round trip, outside timing.
        client.call("RESTORE", key, 0, client.call("DUMP", key), "REPLACE")
    else:
        prefix = "R64" if values[-1] > 0xffffffff else "R"
        client.call(prefix + ".SETINTARRAY", key, *values)
        client.call(prefix + ".OPTIMIZE", key, "MEM")


def workloads(values):
    # Each range workload gives the module's ranks and the equivalent native
    # BIT range, whose ends are the first and last selected offsets.
    count = len(values)
    yield "get-all", None, None, values
    if not count:
        yield "range-empty", (0, 15), (0, 15), []
        return
    middle = count // 2
    for name, start, end in [
        ("range-first16", 0, min(15, count - 1)),
        ("range-middle1pct", middle, min(count - 1, middle + max(1, count // 100) - 1)),
        ("range-last16", max(0, count - 16), count - 1),
        ("range-all", 0, count - 1),
    ]:
        yield name, (start, end), (values[start], values[end]), values[start:end + 1]
    yield "range-empty", (count, count + 15), (values[-1] + 1, values[-1] + 16), []


def command_stats(client, command):
    prefix = "cmdstat_" + command.lower() + ":"
    for line in client.call("INFO", "commandstats").decode().splitlines():
        if line.lower().startswith(prefix):
            return {key: float(value) for key, value in
                    (field.split("=", 1) for field in line[len(prefix):].split(","))}
    return {}


def benchmark(binary, path, command, requests, args):
    invocation = [str(binary), "-s", str(path), "-c", str(args.clients),
                  "-P", str(args.pipeline), "-n", str(requests), "--csv", *map(str, command)]
    result = subprocess.run(invocation, capture_output=True, text=True, timeout=180, check=True)
    # Progress updates use carriage returns; only quoted CSV lines are data.
    lines = [line for line in result.stdout.splitlines() if line.startswith('"')]
    rows = list(csv.DictReader(io.StringIO("\n".join(lines))))
    if len(rows) != 1 or float(rows[0]["rps"]) < 0:
        raise RuntimeError(f"unexpected redis-benchmark output: {result.stdout}")
    sample = {key: float(value) for key, value in rows[0].items() if key != "test"}
    sample["raw_stdout"] = result.stdout
    sample["raw_stderr"] = result.stderr
    return sample


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_revision(src):
    revision = subprocess.run(["git", "-C", str(src.parent), "rev-parse", "HEAD"],
                              capture_output=True, text=True)
    if revision.returncode:
        return None
    dirty = subprocess.run(["git", "-C", str(src.parent), "diff", "--quiet", "HEAD", "--"], check=False)
    return {"commit": revision.stdout.strip(), "tracked_changes": dirty.returncode != 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src-dir", type=Path, default=Path(__file__).resolve().parent.parent / "src")
    parser.add_argument("--module", type=Path, help="optional reference redis-roaring shared library")
    parser.add_argument("--module-src-dir", type=Path,
                        help="Redis binaries for the module baseline process; required with --module")
    parser.add_argument("--module-revision", default=None, help="reference source commit recorded in results")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--requests", type=int, default=5000, help="maximum requests per trial")
    parser.add_argument("--reply-budget-mib", type=int, default=32, help="approximate reply-byte budget per trial")
    parser.add_argument("--trial-seconds", type=float, default=0.5,
                        help="approximate per-trial time cap based on the slower backend's warmup throughput")
    parser.add_argument("--clients", type=int, default=1)
    parser.add_argument("--pipeline", type=int, default=1)
    parser.add_argument("--dataset", action="append", help="restrict dataset names; repeatable")
    parser.add_argument("--json-out", type=Path, required=True)
    args = parser.parse_args()
    if (min(args.trials, args.requests, args.reply_budget_mib, args.clients, args.pipeline) < 1 or
            not math.isfinite(args.trial_seconds) or args.trial_seconds <= 0):
        parser.error("trials, requests, reply budget, clients and pipeline must be positive")
    if bool(args.module) != bool(args.module_src_dir):
        parser.error("--module and --module-src-dir must be supplied together")
    src = args.src_dir.resolve()
    module = args.module.resolve() if args.module else None
    module_src = args.module_src_dir.resolve() if args.module_src_dir else None
    selected = [(name, values) for name, values in datasets() if not args.dataset or name in args.dataset]
    if not selected:
        parser.error("no matching dataset")
    metadata = {
        "platform": platform.platform(), "cpu_count": os.cpu_count(),
        "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "server_sha256": fingerprint(src / "redis-server"),
        "benchmark_sha256": fingerprint(src / "redis-benchmark"),
        "module_sha256": fingerprint(module) if module else None,
        "module_server_sha256": fingerprint(module_src / "redis-server") if module_src else None,
        "native_source": source_revision(src),
        "module_server_source": source_revision(module_src) if module_src else None,
        "module_revision": args.module_revision, "trials": args.trials,
        "clients": args.clients, "pipeline": args.pipeline,
        "max_requests": args.requests, "reply_budget_mib": args.reply_budget_mib,
        "trial_seconds": args.trial_seconds,
        "request_calibration": "slowest warmup throughput; validation fallback for invalid rate",
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    for label, directory in (("native", src), ("module_server", module_src)):
        if directory and (directory / ".make-settings").exists():
            metadata[label + "_make_settings"] = (directory / ".make-settings").read_text()
    cpu_info = Path("/proc/cpuinfo")
    if cpu_info.exists():
        metadata["cpu_model"] = next((line.split(":", 1)[1].strip() for line in
                                      cpu_info.read_text().splitlines() if line.startswith("model name")), None)
    quota = Path("/sys/fs/cgroup/cpu.max")
    metadata["cgroup_cpu_max"] = quota.read_text().strip() if quota.exists() else None
    results = []
    with contextlib.ExitStack() as stack:
        backends = {"native": stack.enter_context(server(src))}
        if module:
            backends["module"] = stack.enter_context(server(module_src, module))
        metadata["server_version"] = subprocess.check_output([str(src / "redis-server"), "--version"], text=True).strip()
        if module_src:
            metadata["module_server_version"] = subprocess.check_output(
                [str(module_src / "redis-server"), "--version"], text=True).strip()
        for dataset, values in selected:
            key = "bench:intarray:" + dataset
            for backend, (client, _) in backends.items():
                seed(client, backend, key, values)
            for workload, ranks, offsets, expected in workloads(values):
                reply_bytes = len(f"*{len(expected)}\r\n") + sum(len(f":{value}\r\n") for value in expected)
                commands = {}
                calibration = {}
                samples = {backend: [] for backend in backends}
                for backend, (client, path) in backends.items():
                    if backend == "native":
                        command = ["BITRANGEINTARRAY", key]
                        if offsets is not None:
                            command += [*offsets, "BIT"]
                        else:
                            command += [0, -1]
                    else:
                        prefix = "R64." if values and values[-1] > 0xffffffff else "R."
                        command = [prefix + ("GETINTARRAY" if ranks is None else "RANGEINTARRAY"), key]
                        if ranks is not None:
                            command += list(ranks)
                    started = time.perf_counter()
                    actual = client.call(*command)
                    calibration[backend] = time.perf_counter() - started
                    if actual != expected:
                        raise RuntimeError(f"incorrect {backend}/{dataset}/{workload} reply")
                    commands[backend] = command
                slowest = max(calibration.values())
                request_cap = max(1, min(args.requests, args.reply_budget_mib * 1048576 // reply_bytes))
                warmup = max(1, min(100, request_cap, int(0.25 / slowest)))
                # redis-benchmark sends complete pipeline batches even when
                # -n is not divisible by -P. Keep command accounting exact.
                warmup = max(args.pipeline, warmup // args.pipeline * args.pipeline)
                warmup_samples = {}
                request_limits = [request_cap]
                for backend, (client, path) in backends.items():
                    command = commands[backend]
                    before = command_stats(client, command[0])
                    sample = benchmark(src / "redis-benchmark", path, command, warmup, args)
                    after = command_stats(client, command[0])
                    if after.get("calls", 0) - before.get("calls", 0) != warmup or any(
                            after.get(field, 0) != before.get(field, 0)
                            for field in ("rejected_calls", "failed_calls")):
                        raise RuntimeError(f"unexpected warmup command accounting for {backend}/{command}")
                    warmup_samples[backend] = sample
                    rate = sample["rps"]
                    if math.isfinite(rate) and rate > 0:
                        request_limits.append(int(args.trial_seconds * rate))
                    elif rate != math.inf:
                        request_limits.append(int(args.trial_seconds / calibration[backend]))
                    # Zero elapsed milliseconds produces a zero rate in this
                    # client, handled by the validation fallback above. Treat
                    # positive infinity defensively as imposing no time cap.
                requests = max(args.pipeline, min(request_limits) // args.pipeline * args.pipeline)
                for trial in range(args.trials):
                    # Alternate order to reduce systematic warm-cache/order bias.
                    order = list(backends) if trial % 2 == 0 else list(reversed(backends))
                    for backend in order:
                        client, path = backends[backend]
                        command = commands[backend]
                        before = command_stats(client, command[0])
                        sample = benchmark(src / "redis-benchmark", path, command, requests, args)
                        after = command_stats(client, command[0])
                        calls = after.get("calls", 0) - before.get("calls", 0)
                        if calls != requests or any(after.get(field, 0) != before.get(field, 0)
                                                    for field in ("rejected_calls", "failed_calls")):
                            raise RuntimeError(f"unexpected command accounting for {backend}/{command}: {before} -> {after}")
                        sample["server_usec_per_call"] = (after["usec"] - before.get("usec", 0)) / calls
                        samples[backend].append(sample)
                for backend in backends:
                    metrics = [metric for metric, value in samples[backend][0].items()
                               if isinstance(value, (float, int))]
                    median = {metric: statistics.median(sample[metric] for sample in samples[backend])
                              for metric in metrics}
                    spread = {}
                    for metric in metrics:
                        measurements = [sample[metric] for sample in samples[backend]]
                        spread[metric] = {"mean": statistics.mean(measurements),
                                          "stdev": statistics.stdev(measurements) if len(measurements) > 1 else 0,
                                          "min": min(measurements), "max": max(measurements)}
                    row = {"backend": backend, "dataset": dataset, "cardinality": len(values),
                           "workload": workload, "command": commands[backend], "returned": len(expected),
                           "reply_bytes": reply_bytes, "requests": requests,
                           "warmup_requests": warmup, "calibration_seconds": calibration[backend],
                           "warmup_sample": warmup_samples[backend],
                           "median": median, "spread": spread, "samples": samples[backend]}
                    results.append(row)
                    print(f"{backend:6} {dataset:16} {workload:17} "
                          f"{median['rps']:10.1f} req/s {median['server_usec_per_call']:9.3f} server-us "
                          f"{len(expected):6} ints", flush=True)
                args.json_out.write_text(json.dumps({"metadata": metadata, "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
