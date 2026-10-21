#!/usr/bin/env python3
"""Measure single-command latency around sparse-bucket promotion."""

import argparse
import csv
import importlib.util
import re
import statistics
import subprocess
import tempfile
import time
import os

benchmark_path = os.path.join(os.path.dirname(__file__), "benchmark-hybrid-roaring-set.py")
spec = importlib.util.spec_from_file_location("hybrid_benchmark", benchmark_path)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)
Client = benchmark.Client
free_port = benchmark.free_port
wait_for_server = benchmark.wait_for_server


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, int(len(values) * fraction))]


def command_latency(client, key, value):
    start = time.perf_counter_ns()
    client.command("SADD", key, value)
    return (time.perf_counter_ns() - start) / 1000


def run_phase(client, key, values):
    client.command("CONFIG", "RESETSTAT")
    latencies = [command_latency(client, key, value) for value in values]
    info = client.command("INFO", "commandstats").decode()
    sadd = next((line for line in info.splitlines() if line.startswith("cmdstat_sadd:")), "")
    average = re.search(r"usec_per_call=([0-9.]+)", sadd)
    percentiles = re.search(r"latency_percentiles_usec=\{([^}]*)\}", sadd)
    server_percentiles = {}
    if percentiles:
        for pair in percentiles.group(1).split(","):
            match = re.fullmatch(r"p([0-9.]+)=([0-9.]+)", pair)
            if match:
                server_percentiles[match.group(1)] = float(match.group(2))
    return latencies, float(average.group(1)) if average else None, server_percentiles


def run(binary, label, roaring, thresholds, repetitions):
    port = free_port()
    directory = tempfile.mkdtemp(prefix=f"redis-roaring-promotion-{label}-")
    args = [binary, "--port", str(port), "--bind", "127.0.0.1",
            "--protected-mode", "no", "--save", "", "--appendonly", "no",
            "--dir", directory, "--loglevel", "warning",
            "--set-max-listpack-entries", "2", "--set-max-intset-entries", "2"]
    if roaring:
        args.extend(("--set-roaring-enabled", "yes"))
    process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client = Client(port)
    results = []
    try:
        wait_for_server(client, process)
        client.command("CONFIG", "SET", "latency-tracking", "yes")
        client.command("CONFIG", "SET", "latency-tracking-info-percentiles", "50 95 99 99.9")
        for threshold in thresholds:
            client.command("FLUSHDB")
            key = f"set-{threshold}"
            # Force a mixed set through the normal listpack promotion path into
            # hybrid encoding before timing integer group insertions.
            client.command("SADD", key, "0")
            client.command("SADD", key, "seed")
            client.command("SADD", key, "1")
            encoding = client.command("OBJECT", "ENCODING", key).decode()
            if roaring and encoding != "hybrid-roaring":
                raise RuntimeError(f"expected hybrid-roaring, got {encoding}")

            # Create independent groups already at occupancy threshold - 2.
            # Time the last cheap insertion and the insertion that promotes.
            commands = []
            for group in range(1, repetitions + 1):
                prefix = group * 65536
                for low in range(threshold - 2):
                    commands.append(("SADD", key, str(prefix + low)))
            client.pipeline(commands)

            before_values = [group * 65536 + threshold - 2
                             for group in range(1, repetitions + 1)]
            crossing_values = [group * 65536 + threshold - 1
                               for group in range(1, repetitions + 1)]
            before, before_server_avg, before_server_pct = run_phase(
                client, key, before_values)
            crossing, crossing_server_avg, crossing_server_pct = run_phase(
                client, key, crossing_values)
            post_values = [group * 65536 + low
                           for group in range(1, repetitions + 1)
                           for low in range(threshold, 2 * threshold)]
            post, post_server_avg, post_server_pct = run_phase(
                client, key, post_values)
            results.append({
                "build": label,
                "compile_threshold": label if roaring else "baseline",
                "group_occupancy": threshold,
                "repetitions": repetitions,
                "before_p50_us": round(statistics.median(before), 2),
                "before_p95_us": round(percentile(before, 0.95), 2),
                "before_p99_us": round(percentile(before, 0.99), 2),
                "before_server_avg_us": before_server_avg,
                "before_server_p99_us": before_server_pct.get("99"),
                "crossing_p50_us": round(statistics.median(crossing), 2),
                "crossing_p95_us": round(percentile(crossing, 0.95), 2),
                "crossing_p99_us": round(percentile(crossing, 0.99), 2),
                "crossing_max_us": round(max(crossing), 2),
                "crossing_server_avg_us": crossing_server_avg,
                "crossing_server_p99_us": crossing_server_pct.get("99"),
                "post_p50_us": round(statistics.median(post), 2),
                "post_p95_us": round(percentile(post, 0.95), 2),
                "post_p99_us": round(percentile(post, 0.99), 2),
                "post_server_avg_us": post_server_avg,
                "post_server_p99_us": post_server_pct.get("99"),
            })
    finally:
        client.close()
        process.terminate()
        process.wait(timeout=10)
        for name in os.listdir(directory):
            os.unlink(os.path.join(directory, name))
        os.rmdir(directory)
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", action="append", required=True,
                        help="THRESHOLD=PATH, repeat for each build")
    parser.add_argument("--out", required=True)
    parser.add_argument("--repetitions", type=int, default=100)
    args = parser.parse_args()

    rows = run(args.baseline, "baseline", False, [16, 32, 64, 128], args.repetitions)
    for item in args.candidate:
        threshold, path = item.split("=", 1)
        threshold = int(threshold)
        rows.extend(run(path, str(threshold), True, [threshold], args.repetitions))

    fields = ["build", "compile_threshold", "group_occupancy", "repetitions",
              "before_p50_us", "before_p95_us", "before_p99_us",
              "before_server_avg_us", "before_server_p99_us",
              "crossing_p50_us", "crossing_p95_us", "crossing_p99_us",
              "crossing_max_us", "crossing_server_avg_us", "crossing_server_p99_us",
              "post_p50_us", "post_p95_us", "post_p99_us",
              "post_server_avg_us", "post_server_p99_us"]
    with open(args.out, "w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print(row)


if __name__ == "__main__":
    main()
