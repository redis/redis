#!/usr/bin/env python3
"""Compare mixed Redis set workloads on a baseline and a Roaring candidate."""

import argparse
import csv
import os
import random
import socket
import subprocess
import tempfile
import time


def encode_command(*parts):
    out = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        value = str(part).encode()
        out.extend((f"${len(value)}\r\n".encode(), value, b"\r\n"))
    return b"".join(out)


def read_reply(reader):
    prefix = reader.read(1)
    if prefix in (b"+", b":"):
        return reader.readline()[:-2]
    if prefix == b"-":
        raise RuntimeError(reader.readline()[:-2].decode(errors="replace"))
    if prefix == b"$":
        size = int(reader.readline()[:-2])
        if size < 0:
            return None
        data = reader.read(size + 2)
        return data[:-2]
    if prefix == b"*":
        count = int(reader.readline()[:-2])
        return [read_reply(reader) for _ in range(count)] if count >= 0 else None
    raise RuntimeError(f"Unknown RESP prefix {prefix!r}")


class Client:
    def __init__(self, port):
        for _ in range(200):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise TimeoutError("Could not connect to Redis")
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = self.sock.makefile("rb")

    def command(self, *args):
        self.sock.sendall(encode_command(*args))
        return read_reply(self.reader)

    def pipeline(self, commands):
        self.sock.sendall(b"".join(encode_command(*command) for command in commands))
        return [read_reply(self.reader) for _ in commands]

    def close(self):
        self.sock.close()


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_for_server(client, process):
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError("Redis process exited during startup")
        try:
            client.command("PING")
            return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError("Redis did not start")


def make_values(n, ratio, distribution):
    int_count = max(1, round(n * ratio))
    if distribution == "dense":
        ints = list(range(int_count))
    elif distribution == "negative-dense":
        ints = [-(i + 1) for i in range(int_count)]
    elif distribution == "sparse":
        ints = [i * 1000003 for i in range(int_count)]
    elif distribution == "negative":
        ints = [-(i + 1) * 1000003 for i in range(int_count)]
    elif distribution.startswith("grouped-"):
        occupancy = int(distribution.split("-", 1)[1])
        ints = [(i // occupancy) * 65536 + (i % occupancy) for i in range(int_count)]
    elif distribution.startswith("negative-grouped-"):
        occupancy = int(distribution.rsplit("-", 1)[1])
        ints = [-((i // occupancy) * 65536 + (i % occupancy) + 1) for i in range(int_count)]
    else:
        ints = [-(1 << 62) + i * 1000003 for i in range(int_count)]
    strings = [f"member-{i:08d}" for i in range(n - int_count)]
    return [str(i) for i in ints] + strings


def elapsed_rate(client, commands, repeats=1):
    start = time.perf_counter()
    for _ in range(repeats):
        client.pipeline(commands)
    elapsed = time.perf_counter() - start
    return len(commands) * repeats / elapsed


def latency_stats(client, command, samples=100):
    latencies = []
    for _ in range(samples):
        start = time.perf_counter_ns()
        client.command(*command)
        latencies.append((time.perf_counter_ns() - start) / 1000)
    latencies.sort()
    return latencies[samples // 2], latencies[min(samples - 1, int(samples * .95))]


def run_binary(label, binary, roaring, csv_writer, sizes, ratios, distributions):
    port = free_port()
    directory = tempfile.mkdtemp(prefix=f"redis-roaring-{label}-")
    args = [binary, "--port", str(port), "--bind", "127.0.0.1", "--protected-mode", "no",
            "--save", "", "--appendonly", "no", "--dir", directory,
            "--loglevel", "warning"]
    if roaring:
        args.extend(("--set-roaring-enabled", "yes"))
    process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client = Client(port)
    try:
        wait_for_server(client, process)
        random.seed(1)
        for n in sizes:
            for ratio in ratios:
                for distribution in distributions:
                    client.command("FLUSHDB")
                    values = make_values(n, ratio, distribution)
                    load_commands = [("SADD", "source", member) for member in values]
                    load_rate = elapsed_rate(client, load_commands)
                    encoding = client.command("OBJECT", "ENCODING", "source").decode()
                    memory = int(client.command("MEMORY", "USAGE", "source"))

                    members = random.sample(values, min(1000, len(values)))
                    probe_commands = [("SISMEMBER", "source", member) for member in members]
                    member_rate = elapsed_rate(client, probe_commands, repeats=3)

                    add_commands = [("SADD", "source", f"new-{i}") for i in range(500)]
                    add_rate = elapsed_rate(client, add_commands)
                    remove_commands = [("SREM", "source", f"new-{i}") for i in range(500)]
                    remove_rate = elapsed_rate(client, remove_commands)

                    iter_start = time.perf_counter()
                    iter_count = len(client.command("SMEMBERS", "source"))
                    iter_seconds = time.perf_counter() - iter_start
                    client.command("SADD", "other", *values[::2])
                    algebra_us = {}
                    for operation, destination in (("SUNIONSTORE", "union"),
                                                   ("SDIFFSTORE", "difference"),
                                                   ("SINTERSTORE", "intersection")):
                        timings = []
                        for _ in range(5):
                            algebra_start = time.perf_counter()
                            client.command(operation, destination, "source", "other")
                            timings.append((time.perf_counter() - algebra_start) * 1e6)
                        algebra_us[operation.lower()] = sum(timings) / len(timings)
                    median_us, p95_us = latency_stats(client, ("SISMEMBER", "source", values[0]))

                    csv_writer.writerow({
                        "build": label, "cardinality": n, "integer_ratio": ratio,
                        "distribution": distribution, "encoding": encoding,
                        "bytes_per_member": round(memory / n, 2),
                        "add_ops_sec": round(load_rate), "membership_ops_sec": round(member_rate),
                        "incremental_add_ops_sec": round(add_rate),
                        "remove_ops_sec": round(remove_rate),
                        "smembers_members_sec": round(iter_count / iter_seconds),
                        "sunionstore_us": round(algebra_us["sunionstore"], 1),
                        "sdiffstore_us": round(algebra_us["sdiffstore"], 1),
                        "sinterstore_us": round(algebra_us["sinterstore"], 1),
                        "sismember_median_us": round(median_us, 1),
                        "sismember_p95_us": round(p95_us, 1),
                    })
                    print(f"{label} n={n} ints={ratio:.0%} {distribution} {encoding}: "
                          f"{memory / n:.1f} B/member, {member_rate:.0f} membership ops/s")
    finally:
        client.close()
        process.terminate()
        process.wait(timeout=10)
        for name in os.listdir(directory):
            os.unlink(os.path.join(directory, name))
        os.rmdir(directory)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--sizes", default="256,4096,32768")
    parser.add_argument("--ratios", default="0.9,0.5,0.1")
    parser.add_argument("--distributions", default="dense,negative-dense,sparse,negative,high")
    args = parser.parse_args()
    sizes = [int(value) for value in args.sizes.split(",")]
    ratios = [float(value) for value in args.ratios.split(",")]
    distributions = args.distributions.split(",")
    with open(args.out, "w", newline="") as output:
        fields = ["build", "cardinality", "integer_ratio", "distribution", "encoding",
                  "bytes_per_member", "add_ops_sec", "membership_ops_sec",
                  "incremental_add_ops_sec", "remove_ops_sec", "smembers_members_sec",
                  "sunionstore_us", "sdiffstore_us", "sinterstore_us",
                  "sismember_median_us", "sismember_p95_us"]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        run_binary("baseline", args.baseline, False, writer, sizes, ratios, distributions)
        run_binary("candidate", args.candidate, True, writer, sizes, ratios, distributions)


if __name__ == "__main__":
    main()
