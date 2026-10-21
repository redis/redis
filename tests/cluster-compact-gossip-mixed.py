#!/usr/bin/env python3
"""Manual rolling-upgrade check for short Cluster heartbeats and gossip.

Example:
  python3 tests/cluster-compact-gossip-mixed.py \
      --old-server /path/to/baseline/src/redis-server \
      --new-server /path/to/changed/src/redis-server

The test starts four local nodes and a TCP forwarder for each cluster bus port.
It needs two independently built binaries, so it is not part of the Tcl suite.
All processes and temporary files are removed even if an assertion fails.
"""

import argparse
import binascii
import contextlib
import dataclasses
import os
import random
import select
import signal
import socket
import socketserver
import statistics
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path


HOST = "127.0.0.1"
BUS_OFFSET = 10000
PROXY_OFFSET = 20000
LEGACY_HEADER_SIZE = 2256
SHORT_HEADER_SIZE = 208
SLOT_BITMAP_SIZE = LEGACY_HEADER_SIZE - SHORT_HEADER_SIZE
LEGACY_GOSSIP_SIZE = 104
COMPACT_EXT_TYPE = 5
INDEXED_EXT_TYPE = 6


class RedisError(RuntimeError):
    pass


def redis_command(port, *args, timeout=10):
    encoded = [str(arg).encode() if not isinstance(arg, bytes) else arg for arg in args]
    request = b"*%d\r\n" % len(encoded)
    request += b"".join(b"$%d\r\n%s\r\n" % (len(arg), arg) for arg in encoded)

    def parse(stream):
        line = stream.readline()
        if not line.endswith(b"\r\n"):
            raise RedisError("incomplete RESP reply")
        prefix, value = line[:1], line[1:-2]
        if prefix == b"+":
            return value.decode()
        if prefix == b"-":
            raise RedisError(value.decode(errors="replace"))
        if prefix == b":":
            return int(value)
        if prefix == b"$":
            length = int(value)
            if length < 0:
                return None
            data = stream.read(length + 2)
            if len(data) != length + 2 or not data.endswith(b"\r\n"):
                raise RedisError("incomplete bulk reply")
            return data[:-2]
        if prefix == b"*":
            length = int(value)
            return None if length < 0 else [parse(stream) for _ in range(length)]
        raise RedisError("unknown RESP reply")

    with socket.create_connection((HOST, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request)
        with sock.makefile("rb") as stream:
            return parse(stream)


def text_reply(value):
    return value.decode() if isinstance(value, bytes) else value


def cluster_info(port):
    lines = text_reply(redis_command(port, "CLUSTER", "INFO")).splitlines()
    return dict(line.split(":", 1) for line in lines if ":" in line)


def cluster_nodes(port):
    result = {}
    for line in text_reply(redis_command(port, "CLUSTER", "NODES")).splitlines():
        fields = line.split()
        if len(fields) >= 8:
            result[fields[0]] = fields
    return result


def wait_for(label, predicate, seconds=45):
    deadline = time.monotonic() + seconds
    last_error = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, RedisError, ValueError, KeyError) as exc:
            last_error = exc
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {label}: {last_error}")


@dataclasses.dataclass(frozen=True)
class Frame:
    when: float
    sender: str
    target: int
    connection: int
    version: int
    kind: int
    length: int
    legacy_count: int
    compact_count: int
    indexed_count: int


class Recorder:
    def __init__(self):
        self.lock = threading.Lock()
        self.frames = []
        self.next_connection = 0

    def new_connection(self):
        with self.lock:
            self.next_connection += 1
            return self.next_connection

    def add(self, frame):
        with self.lock:
            self.frames.append(frame)

    def select(self, since, sender, target):
        with self.lock:
            return [frame for frame in self.frames if frame.when >= since
                    and frame.sender == sender and frame.target == target
                    and frame.kind in (0, 1)]


class FrameParser:
    def __init__(self, target, recorder, connection):
        self.target = target
        self.recorder = recorder
        self.connection = connection
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        while len(self.buffer) >= 8:
            if self.buffer[:4] != b"RCmb":
                self.buffer.clear()
                return
            length = struct.unpack_from(">I", self.buffer, 4)[0]
            if length < SHORT_HEADER_SIZE or length > 16 * 1024 * 1024:
                self.buffer.clear()
                return
            if len(self.buffer) < length:
                return
            packet = bytes(self.buffer[:length])
            del self.buffer[:length]
            version = struct.unpack_from(">H", packet, 8)[0]
            if version not in (1, 2):
                continue
            header_size = LEGACY_HEADER_SIZE if version == 1 else SHORT_HEADER_SIZE
            if length < header_size:
                continue
            kind, legacy_count = struct.unpack_from(">HH", packet, 12)
            compact_count = 0
            indexed_count = 0
            shift = 0 if version == 1 else SLOT_BITMAP_SIZE
            if kind in (0, 1, 2) and packet[2253 - shift] & 4:
                ext_count = struct.unpack_from(">H", packet, 2214 - shift)[0]
                offset = header_size + legacy_count * LEGACY_GOSSIP_SIZE
                for _ in range(ext_count):
                    if offset + 8 > length:
                        break
                    ext_len, ext_type = struct.unpack_from(">IH", packet, offset)
                    if ext_len < 8 or offset + ext_len > length:
                        break
                    if ext_type == COMPACT_EXT_TYPE and ext_len >= 10:
                        compact_count = struct.unpack_from(">H", packet, offset + 8)[0]
                    if ext_type == INDEXED_EXT_TYPE and ext_len >= 16:
                        indexed_count = struct.unpack_from(">H", packet, offset + 8)[0]
                    offset += ext_len
            sender = packet[40:80].decode("ascii", errors="replace")
            self.recorder.add(Frame(time.monotonic(), sender, self.target,
                                    self.connection, version, kind, length,
                                    legacy_count, compact_count, indexed_count))


class BusProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, proxy_port, actual_port, target, recorder):
        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                parser = FrameParser(self.server.target, self.server.recorder,
                                     self.server.recorder.new_connection())
                try:
                    with socket.create_connection((HOST, self.server.actual_port), timeout=3) as upstream:
                        peers = (self.request, upstream)
                        while True:
                            readable, _, _ = select.select(peers, [], [], 0.5)
                            for source in readable:
                                data = source.recv(65536)
                                if not data:
                                    return
                                if source is self.request:
                                    parser.feed(data)
                                    upstream.sendall(data)
                                else:
                                    self.request.sendall(data)
                except OSError:
                    return

        super().__init__((HOST, proxy_port), Handler)
        self.actual_port = actual_port
        self.target = target
        self.recorder = recorder
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=2)


def choose_base(count, requested):
    choices = [requested] if requested is not None else [random.randrange(20000, 30000) for _ in range(100)]
    for base in choices:
        sockets = []
        try:
            for offset in (0, BUS_OFFSET, PROXY_OFFSET):
                for index in range(count):
                    sock = socket.socket()
                    sock.bind((HOST, base + offset + index))
                    sockets.append(sock)
            return base
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("could not find free client, bus and proxy ports")


class Cluster:
    def __init__(self, old_server, new_server, base_port=None):
        self.binary = [old_server, new_server, new_server, old_server]
        self.base = choose_base(len(self.binary), base_port)
        self.temp = tempfile.TemporaryDirectory(prefix="redis-compact-mixed-")
        self.root = Path(self.temp.name)
        self.recorder = Recorder()
        self.proxies = []
        self.processes = []
        self.logs = []

    def port(self, index):
        return self.base + index

    def proxy_port(self, index):
        return self.port(index) + PROXY_OFFSET

    def command(self, index, *args):
        return redis_command(self.port(index), *args)

    def start(self):
        for index in range(len(self.binary)):
            self.proxies.append(BusProxy(self.proxy_port(index),
                                         self.port(index) + BUS_OFFSET,
                                         index, self.recorder))
        for index, binary in enumerate(self.binary):
            directory = self.root / str(index)
            directory.mkdir()
            config = directory / "redis.conf"
            logfile = directory / "redis.log"
            config.write_text("\n".join([
                f"port {self.port(index)}",
                f"bind {HOST}",
                "protected-mode no",
                "cluster-enabled yes",
                f"cluster-config-file {directory / 'nodes.conf'}",
                "cluster-node-timeout 3000",
                "cluster-ping-interval 100",
                "cluster-bus-port-protected-mode no",
                f"cluster-announce-ip {HOST}",
                f"cluster-announce-bus-port {self.proxy_port(index)}",
                "enable-debug-command yes",
                "appendonly no",
                f"dir {directory}",
                f"logfile {logfile}",
            ]) + "\n")
            output = open(directory / "startup.log", "wb")
            self.logs.append(output)
            self.processes.append(subprocess.Popen([str(binary), str(config)],
                                                    stdout=output, stderr=subprocess.STDOUT))
        for index, process in enumerate(self.processes):
            wait_for(f"node {index} startup", lambda i=index, p=process:
                     self._alive(i, p), seconds=15)

    def _alive(self, index, process):
        if process.poll() is not None:
            log = (self.root / str(index) / "redis.log")
            raise RuntimeError(f"node {index} exited: {log.read_text(errors='replace')[-2000:] if log.exists() else ''}")
        return self.command(index, "PING") == "PONG"

    def close(self):
        for process in self.processes:
            if process.poll() is None and hasattr(signal, "SIGCONT"):
                with contextlib.suppress(ProcessLookupError):
                    os.kill(process.pid, signal.SIGCONT)
        for index in range(len(self.processes)):
            with contextlib.suppress(OSError, RedisError):
                self.command(index, "SHUTDOWN", "NOSAVE")
        for process in self.processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        for proxy in self.proxies:
            proxy.close()
        for output in self.logs:
            output.close()
        self.temp.cleanup()


def ids(cluster):
    return [text_reply(cluster.command(index, "CLUSTER", "MYID")) for index in range(4)]


def all_members_visible(cluster, expected_ids):
    for index in range(4):
        if cluster_info(cluster.port(index)).get("cluster_known_nodes") != "4":
            return False
        if set(cluster_nodes(cluster.port(index))) != set(expected_ids):
            return False
    return True


def all_links_connected(cluster, expected_ids):
    for index in range(4):
        members = cluster_nodes(cluster.port(index))
        if set(members) != set(expected_ids):
            return False
        if any(fields[7] != "connected" for node, fields in members.items()
               if node != expected_ids[index]):
            return False
    return True


def key_for_range(first, last):
    for number in range(100000):
        key = f"mixed{{{number}}}"
        slot = binascii.crc_hqx(str(number).encode(), 0) % 16384
        if first <= slot <= last:
            return key
    raise AssertionError("could not find a key for slot range")


def run(cluster, timeout):
    cluster.start()
    node_ids = ids(cluster)
    assert len(set(node_ids)) == 4
    print(f"client ports: {[cluster.port(i) for i in range(4)]}")

    # Chain MEETs so full membership must propagate beyond the direct peer.
    for source, target in ((0, 1), (1, 2), (2, 3)):
        assert cluster.command(source, "CLUSTER", "MEET", HOST,
                               cluster.port(target), cluster.proxy_port(target)) == "OK"
    wait_for("no-slot discovery through mixed peers",
             lambda: all_members_visible(cluster, node_ids), timeout)
    wait_for("all cluster links connected",
             lambda: all_links_connected(cluster, node_ids), timeout)
    print("PASS: all old and new nodes discovered one another without slots")

    since = time.monotonic()
    def captured_initial():
        new_new = cluster.recorder.select(since, node_ids[1], 2)
        new_old = cluster.recorder.select(since, node_ids[1], 0)
        old_new = cluster.recorder.select(since, node_ids[0], 1)
        short_counts = {f.indexed_count + f.compact_count for f in new_new
                        if f.version == 2}
        legacy_counts = {f.legacy_count for f in new_old if f.version == 1}
        return (sum(f.version == 2 and f.length < LEGACY_HEADER_SIZE
                    for f in new_new) >= 3
                and sum(f.version == 1 and f.legacy_count > 0 for f in new_old) >= 3
                and sum(f.version == 1 and f.legacy_count > 0 for f in old_new) >= 3
                and bool((short_counts & legacy_counts) - {0}))
    wait_for("new/new short and mixed legacy frames", captured_initial, timeout)
    new_new = cluster.recorder.select(since, node_ids[1], 2)
    new_old = cluster.recorder.select(since, node_ids[1], 0)
    old_new = cluster.recorder.select(since, node_ids[0], 1)
    assert all(f.version == 1 and f.compact_count == 0 for f in new_old + old_new)
    short_frames = [f for f in new_new if f.version == 2]
    assert all(f.legacy_count == 0 and f.length < LEGACY_HEADER_SIZE for f in short_frames)
    assert any(f.indexed_count > 0 for f in short_frames), \
        "short frames never carried indexed gossip"
    legacy_frames = [f for f in new_old if f.version == 1 and f.legacy_count > 0]
    short_counts = {f.indexed_count + f.compact_count for f in short_frames}
    legacy_counts = {f.legacy_count for f in legacy_frames}
    common_count = max((short_counts & legacy_counts) - {0})
    short_median = statistics.median(
        f.length for f in short_frames
        if f.indexed_count + f.compact_count == common_count)
    legacy_median = statistics.median(
        f.length for f in legacy_frames if f.legacy_count == common_count)
    assert short_median < legacy_median / 2, (short_median, legacy_median)
    print(f"PASS: {common_count} gossip entries, new→new short heartbeat median {short_median:g} B; "
          f"new→old legacy median {legacy_median:g} B")

    # A new peer must re-negotiate after links to new and old peers are killed.
    old_connections = {target: {f.connection for f in cluster.recorder.select(0, node_ids[1], target)}
                       for target in (2, 0)}
    for target in (2, 0):
        cluster.command(1, "DEBUG", "CLUSTERLINK", "KILL", "TO", node_ids[target])
    wait_for("reconnected mixed links", lambda: all_links_connected(cluster, node_ids), timeout)
    def fresh_frames(target):
        return [f for f in cluster.recorder.select(0, node_ids[1], target)
                if f.connection not in old_connections[target]]
    wait_for("short negotiation after reconnect", lambda:
             any(f.version == 2 for f in fresh_frames(2))
             and any(f.version == 1 and f.legacy_count > 0 for f in fresh_frames(0)), timeout)
    new_link_frames = fresh_frames(2)
    assert new_link_frames[0].version == 1, "new link sent short frame before v1 negotiation"
    assert all(f.version == 1 and f.compact_count == 0 for f in fresh_frames(0))
    print("PASS: reconnect renegotiated v1→v2 between new peers and retained v1 for old peer")

    ranges = ((0, 5460), (5461, 10922), (10923, 16383))
    for index, (first, last) in zip((0, 1, 3), ranges):
        cluster.command(index, "CLUSTER", "ADDSLOTS", *range(first, last + 1))
    cluster.command(2, "CLUSTER", "REPLICATE", node_ids[0])
    wait_for("cluster_state ok on every node", lambda:
             all(cluster_info(cluster.port(i)).get("cluster_state") == "ok" for i in range(4)), timeout)
    wait_for("new replica connected to old master", lambda:
             b"master_link_status:up" in cluster.command(2, "INFO", "replication"), timeout)

    key = key_for_range(*ranges[0])
    assert cluster.command(0, "SET", key, "mixed-version-value") == "OK"
    assert cluster.command(0, "WAIT", 1, 5000) >= 1
    assert cluster.command(0, "GET", key) == b"mixed-version-value"
    print("PASS: slots, cross-version replication and client write/read")

    if not hasattr(signal, "SIGSTOP"):
        raise RuntimeError("SIGSTOP is needed for the FAIL/recovery check")
    old_master = cluster.processes[0]
    os.kill(old_master.pid, signal.SIGSTOP)
    wait_for("old master marked FAIL", lambda:
             "fail" in cluster_nodes(cluster.port(1))[node_ids[0]][2].split(","), timeout)
    wait_for("new replica promoted", lambda:
             "master" in cluster_nodes(cluster.port(1))[node_ids[2]][2].split(",")
             and cluster.command(2, "GET", key) == b"mixed-version-value", timeout)
    os.kill(old_master.pid, signal.SIGCONT)
    wait_for("old master recovered and all nodes healthy", lambda:
             all(cluster_info(cluster.port(i)).get("cluster_state") == "ok" for i in range(4))
             and all_members_visible(cluster, node_ids)
             and all("fail" not in cluster_nodes(cluster.port(i))[node_ids[0]][2].split(",")
                     for i in range(4)), timeout)
    assert cluster.command(2, "GET", key) == b"mixed-version-value"
    print("PASS: slotted old master failed, new replica promoted, and old node recovered")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-server", required=True, type=Path)
    parser.add_argument("--new-server", required=True, type=Path)
    parser.add_argument("--base-port", type=int, help="client base port, with bus +10000 and proxy +20000")
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    for binary in (args.old_server, args.new_server):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"not an executable redis-server: {binary}")
    if args.base_port is not None and not 1024 <= args.base_port <= 45000:
        parser.error("--base-port must be between 1024 and 45000")

    cluster = Cluster(args.old_server.resolve(), args.new_server.resolve(), args.base_port)
    try:
        run(cluster, args.timeout)
    finally:
        cluster.close()


if __name__ == "__main__":
    main()
