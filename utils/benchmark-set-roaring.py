#!/usr/bin/env python3
"""Compare pure-integer set encodings on baseline and Roaring-enabled Redis."""

import argparse
import csv
import os
import socket
import subprocess
import tempfile
import time


def encode(*parts):
    payload = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        value = str(part).encode()
        payload.extend((f"${len(value)}\r\n".encode(), value, b"\r\n"))
    return b"".join(payload)


def read_reply(reader):
    prefix = reader.read(1)
    if prefix in (b"+", b":"):
        return reader.readline()[:-2]
    if prefix == b"-":
        raise RuntimeError(reader.readline()[:-2].decode(errors="replace"))
    if prefix == b"$":
        size = int(reader.readline()[:-2])
        return None if size < 0 else reader.read(size + 2)[:-2]
    if prefix == b"*":
        count = int(reader.readline()[:-2])
        return [read_reply(reader) for _ in range(count)] if count >= 0 else None
    raise RuntimeError(f"Unexpected RESP prefix {prefix!r}")


class Client:
    def __init__(self, port):
        for _ in range(200):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise TimeoutError("Redis did not start")
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = self.sock.makefile("rb")

    def command(self, *args):
        self.sock.sendall(encode(*args))
        return read_reply(self.reader)

    def pipeline(self, commands):
        self.sock.sendall(b"".join(encode(*command) for command in commands))
        return [read_reply(self.reader) for _ in commands]

    def close(self):
        self.reader.close()
        self.sock.close()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def values_for(count, pattern):
    if pattern == "dense":
        return [str(i) for i in range(count)]
    if pattern == "negative-dense":
        return [str(-i - 1) for i in range(count)]
    if pattern == "sparse":
        return [str(i * 1000003) for i in range(count)]
    if pattern == "negative-sparse":
        return [str(-(i + 1) * 1000003) for i in range(count)]
    if pattern == "high-range":
        return [str(-(1 << 62) + i * 1000003) for i in range(count)]
    raise ValueError(pattern)


def rate(client, commands, repeats=1):
    start = time.perf_counter()
    for _ in range(repeats):
        client.pipeline(commands)
    return len(commands) * repeats / (time.perf_counter() - start)


def bench(binary, label, roaring, sizes, patterns, writer, run_number):
    port = free_port()
    directory = tempfile.mkdtemp(prefix=f"redis-pure-integer-{label}-")
    args = [binary, "--port", str(port), "--bind", "127.0.0.1", "--protected-mode", "no",
            "--save", "", "--appendonly", "no", "--dir", directory, "--loglevel", "warning"]
    if roaring:
        args.extend(("--set-roaring-enabled", "yes"))
    process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client = Client(port)
    try:
        for _ in range(200):
            if process.poll() is not None:
                raise RuntimeError(f"{label} Redis exited during startup")
            try:
                if client.command("PING") == b"PONG":
                    break
            except OSError:
                time.sleep(0.05)
        for size in sizes:
            for pattern in patterns:
                client.command("FLUSHDB")
                values = values_for(size, pattern)
                load_commands = [("SADD", "source", value) for value in values]
                started = time.perf_counter()
                client.pipeline(load_commands)
                add_rate = size / (time.perf_counter() - started)
                encoding = client.command("OBJECT", "ENCODING", "source").decode()
                memory = int(client.command("MEMORY", "USAGE", "source"))

                probes = values[::max(1, size // 1000)]
                member_rate = rate(client, [("SISMEMBER", "source", value) for value in probes], 3)
                iter_start = time.perf_counter()
                iterated = len(client.command("SMEMBERS", "source"))
                iteration_rate = iterated / (time.perf_counter() - iter_start)

                client.command("SADD", "other", *values[::2])
                algebra = {}
                for operation, destination in (("SUNIONSTORE", "union"),
                                               ("SDIFFSTORE", "difference"),
                                               ("SINTERSTORE", "intersection")):
                    samples = []
                    for _ in range(5):
                        start = time.perf_counter()
                        client.command(operation, destination, "source", "other")
                        samples.append((time.perf_counter() - start) * 1e6)
                    algebra[operation.lower()] = sum(samples) / len(samples)

                incremental_values = [str((1 << 60) + i) for i in range(500)]
                incremental_add_rate = rate(
                    client, [("SADD", "source", value) for value in incremental_values])
                incremental_remove_rate = rate(
                    client, [("SREM", "source", value) for value in incremental_values])

                writer.writerow({
                    "run": run_number, "build": label, "cardinality": size, "pattern": pattern,
                    "encoding": encoding, "bytes_per_member": round(memory / size, 2),
                    "add_ops_sec": round(add_rate), "membership_ops_sec": round(member_rate),
                    "incremental_add_ops_sec": round(incremental_add_rate),
                    "remove_ops_sec": round(incremental_remove_rate),
                    "iteration_members_sec": round(iteration_rate),
                    "sunionstore_us": round(algebra["sunionstore"], 1),
                    "sdiffstore_us": round(algebra["sdiffstore"], 1),
                    "sinterstore_us": round(algebra["sinterstore"], 1),
                })
                print(f"{label} n={size} {pattern} {encoding}: {memory / size:.1f} B/member, "
                      f"add {add_rate:.0f}/s, member {member_rate:.0f}/s")
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
    parser.add_argument("--sizes", default="512,513,4096,32768,100000")
    parser.add_argument("--patterns", default="dense,negative-dense,sparse,negative-sparse,high-range")
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    sizes = [int(value) for value in args.sizes.split(",")]
    patterns = args.patterns.split(",")
    with open(args.out, "w", newline="") as output:
        fields = ["run", "build", "cardinality", "pattern", "encoding", "bytes_per_member",
                  "add_ops_sec", "membership_ops_sec", "incremental_add_ops_sec",
                  "remove_ops_sec", "iteration_members_sec",
                  "sunionstore_us", "sdiffstore_us", "sinterstore_us"]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for run_number in range(1, args.repetitions + 1):
            builds = [(args.baseline, "baseline", False), (args.candidate, "candidate", True)]
            if run_number % 2 == 0:
                builds.reverse()
            for binary, label, roaring in builds:
                bench(binary, label, roaring, sizes, patterns, writer, run_number)


if __name__ == "__main__":
    main()
