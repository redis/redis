#!/usr/bin/env python3
"""Measure idle Redis Cluster bus traffic with an instrumented redis-server.

The server must expose cluster_stats_bytes_sent and
cluster_stats_bytes_received in CLUSTER INFO. Each run creates an isolated,
slotless loopback cluster and stops every server before moving to the next size.

This measures Redis Cluster bus payload bytes, not TCP/TLS framing. A 900-node
full mesh has about 809,100 TCP connections and 1,618,200 socket endpoints;
run that size only on a host provisioned for it. A 40/100-node result must not
be presented as a measured 900-node or 10x result. For comparisons, run an
instrumented legacy build and the changed build with identical options and
alternate their order across repetitions (ABBA).

Example:
  python3 utils/cluster-gossip-benchmark.py --nodes 6 40 100 --duration 30 \
      --server src/redis-server --cli src/redis-cli --output /tmp/gossip.json
"""

import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
BYTE_FIELDS = ("cluster_stats_bytes_sent", "cluster_stats_bytes_received")
MESSAGE_FIELDS = (
    "cluster_stats_messages_ping_sent",
    "cluster_stats_messages_pong_sent",
    "cluster_stats_messages_ping_received",
    "cluster_stats_messages_pong_received",
)
SHORT_MESSAGE_FIELDS = (
    "cluster_stats_bus_short_messages_sent",
    "cluster_stats_bus_short_messages_received",
)
BUFFER_FIELD = "total_cluster_links_buffer_limit_exceeded"


class BenchmarkError(Exception):
    pass


class MissingMetrics(BenchmarkError):
    pass


class CommandTimeout(BenchmarkError):
    pass


def executable(value, label):
    path = Path(value).expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise BenchmarkError(
            f"{label} not found or not executable: {path}. "
            f"Build with 'make -C {REPO / 'src'} redis-server redis-cli', "
            "or pass --server and --cli."
        )
    return path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def redis_cli(cli_path, port, *command, timeout=10):
    argv = [str(cli_path), "--raw", "-h", "127.0.0.1", "-p", str(port), *map(str, command)]
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise CommandTimeout(f"redis-cli timed out after {timeout}s on port {port}: {' '.join(map(str, command))}") from exc
    output = result.stdout.strip()
    if result.returncode != 0 or output.startswith("(error)") or output.startswith("ERR "):
        detail = result.stderr.strip() or output or f"exit status {result.returncode}"
        raise BenchmarkError(f"redis-cli on port {port} failed: {detail}")
    return output


def info_fields(raw):
    fields = {}
    for line in raw.splitlines():
        if ":" in line and not line.startswith("#"):
            key, value = line.split(":", 1)
            fields[key] = value.strip()
    return fields


def cluster_info(cli_path, port):
    return info_fields(redis_cli(cli_path, port, "CLUSTER", "INFO"))


def check_byte_metrics(fields, server_path):
    missing = [name for name in BYTE_FIELDS if name not in fields]
    if missing:
        raise MissingMetrics(
            f"{server_path} does not expose {', '.join(missing)} in CLUSTER INFO. "
            "This baseline binary cannot measure bus bytes; build the instrumented "
            "redis-server or pass its path with --server. All started nodes were stopped."
        )
    for name in BYTE_FIELDS:
        try:
            int(fields[name])
        except ValueError as exc:
            raise BenchmarkError(f"CLUSTER INFO field {name} is not an integer: {fields[name]!r}") from exc


def port_is_available(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def wait_ready(processes, ports, log_paths, deadline):
    while time.monotonic() < deadline:
        for process, port, log_path in zip(processes, ports, log_paths):
            if process.poll() is not None:
                tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-12:])
                raise BenchmarkError(f"redis-server on port {port} exited ({process.returncode}):\n{tail}")
        ready = True
        for port in ports:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.15):
                    pass
            except OSError:
                ready = False
                break
        if ready:
            return
        time.sleep(0.2)
    raise BenchmarkError(f"Timed out waiting for Redis ports {ports[0]}..{ports[-1]}")


def start_server(server_path, port, timeout_ms, run_dir):
    node_dir = run_dir / str(port)
    node_dir.mkdir()
    log_path = node_dir / "server.log"
    argv = [
        str(server_path), "--port", str(port), "--bind", "127.0.0.1",
        "--protected-mode", "no", "--cluster-enabled", "yes",
        "--cluster-bus-port-protected-mode", "no",
        "--cluster-config-file", "nodes.conf", "--cluster-node-timeout", str(timeout_ms),
        "--cluster-announce-ip", "127.0.0.1", "--dir", str(node_dir),
        "--save", "", "--appendonly", "no", "--loglevel", "notice",
        "--logfile", "", "--daemonize", "no",
    ]
    with log_path.open("wb") as log:
        process = subprocess.Popen(argv, cwd=node_dir, stdout=log, stderr=subprocess.STDOUT)
    return process, log_path


def stop_servers(processes):
    for process in processes:
        if process.poll() is None:
            process.terminate()
    deadline = time.monotonic() + 5
    for process in processes:
        if process.poll() is None:
            try:
                process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
    for process in processes:
        if process.poll() is None:
            process.kill()
    for process in processes:
        process.wait()


def known_nodes(cli_path, port):
    fields = cluster_info(cli_path, port)
    return int(fields["cluster_known_nodes"])


def meet_node(cli_path, new_port, seed_port, timeout_s, retries):
    # Ask the fresh node to contact the seed, so the seed does not have to
    # process every CLUSTER MEET command while it forms many bus links.
    for attempt in range(retries + 1):
        try:
            redis_cli(cli_path, new_port, "CLUSTER", "MEET", "127.0.0.1", seed_port, timeout=timeout_s)
            return
        except CommandTimeout as exc:
            # The command may have succeeded even if redis-cli did not receive
            # its reply before the deadline. Avoid another MEET when it did.
            try:
                if known_nodes(cli_path, new_port) >= 2:
                    return
            except (BenchmarkError, KeyError, ValueError):
                pass
            if attempt == retries:
                raise BenchmarkError(
                    f"CLUSTER MEET for node {new_port} timed out after {retries + 1} attempts"
                ) from exc
            time.sleep(min(1 + attempt, 3))


def wait_converged(cli_path, ports, timeout_s, workers):
    deadline = time.monotonic() + timeout_s
    last_counts = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        while time.monotonic() < deadline:
            try:
                last_counts = list(pool.map(lambda port: known_nodes(cli_path, port), ports))
            except (BenchmarkError, KeyError, ValueError):
                time.sleep(1)
                continue
            if all(count == len(ports) for count in last_counts):
                return
            time.sleep(2)
    observed = f"{min(last_counts)}..{max(last_counts)}" if last_counts else "unavailable"
    raise BenchmarkError(f"Cluster did not converge to {len(ports)} known nodes; observed {observed}")


def connected_peer_count(cli_path, port, expected_nodes):
    lines = redis_cli(cli_path, port, "CLUSTER", "NODES", timeout=30).splitlines()
    if len(lines) != expected_nodes:
        return 0
    peers = 0
    for line in lines:
        fields = line.split()
        if len(fields) < 8:
            return 0
        flags = fields[2].split(",")
        if "myself" in flags:
            continue
        if fields[7] != "connected" or "fail" in flags or "fail?" in flags:
            return 0
        peers += 1
    return peers


def wait_full_mesh(cli_path, ports, timeout_s, workers):
    deadline = time.monotonic() + timeout_s
    last_min = 0
    with ThreadPoolExecutor(max_workers=workers) as pool:
        while time.monotonic() < deadline:
            counts = list(pool.map(lambda port: connected_peer_count(
                cli_path, port, len(ports)), ports))
            last_min = min(counts)
            if last_min == len(ports) - 1:
                return
            time.sleep(2)
    raise BenchmarkError(f"Cluster did not form a connected {len(ports)}-node mesh; "
                         f"minimum connected peers was {last_min}/{len(ports) - 1}")


def sample_node(cli_path, server_path, port):
    cluster = cluster_info(cli_path, port)
    check_byte_metrics(cluster, server_path)
    cpu = info_fields(redis_cli(cli_path, port, "INFO", "CPU"))
    if "used_cpu_user_main_thread" in cpu and "used_cpu_sys_main_thread" in cpu:
        cpu_s = float(cpu["used_cpu_user_main_thread"]) + float(cpu["used_cpu_sys_main_thread"])
        scope = "main_thread"
    else:
        cpu_s = float(cpu["used_cpu_user"]) + float(cpu["used_cpu_sys"])
        scope = "process"
    result = {name: int(cluster.get(name, "0")) for name in
              (*BYTE_FIELDS, *MESSAGE_FIELDS, *SHORT_MESSAGE_FIELDS, BUFFER_FIELD)}
    result.update(port=port, known_nodes=int(cluster["cluster_known_nodes"]), cpu_s=cpu_s, cpu_scope=scope)
    return result


def snapshot(cli_path, server_path, ports, workers):
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        rows = list(pool.map(lambda port: sample_node(cli_path, server_path, port), ports))
    end = time.monotonic()
    return rows, (start + end) / 2, end - start


def run_cluster(args, server_path, cli_path, count):
    ports = list(range(args.base_port, args.base_port + count))
    for port in ports:
        for listen_port in (port, port + 10000):
            if not port_is_available(listen_port):
                raise BenchmarkError(f"Port {listen_port} is already in use")

    processes = []
    log_paths = []
    with tempfile.TemporaryDirectory(prefix=f"redis-gossip-{count}-") as temporary:
        run_dir = Path(temporary)
        try:
            # Check instrumentation after only one node has started, before
            # creating a large cluster with a baseline binary.
            process, log_path = start_server(server_path, ports[0], args.timeout_ms, run_dir)
            processes.append(process)
            log_paths.append(log_path)
            wait_ready(processes, ports[:1], log_paths, time.monotonic() + args.startup_timeout)
            check_byte_metrics(cluster_info(cli_path, ports[0]), server_path)

            for port in ports[1:]:
                process, log_path = start_server(server_path, port, args.timeout_ms, run_dir)
                processes.append(process)
                log_paths.append(log_path)
            wait_ready(processes, ports, log_paths, time.monotonic() + args.startup_timeout)

            for index, port in enumerate(ports[1:], start=1):
                meet_node(cli_path, port, ports[0], args.meet_timeout, args.meet_retries)
                if index % 10 == 0 or index == count - 1:
                    print(f"  CLUSTER MEET {index}/{count - 1}", file=sys.stderr, flush=True)
                if args.meet_delay:
                    time.sleep(args.meet_delay)
            wait_converged(cli_path, ports, args.convergence_timeout, args.workers)
            wait_full_mesh(cli_path, ports, args.convergence_timeout, args.workers)
            time.sleep(args.warmup)
            wait_full_mesh(cli_path, ports, args.convergence_timeout, args.workers)

            before, before_midpoint, before_span = snapshot(cli_path, server_path, ports, args.workers)
            time.sleep(args.duration)
            after, after_midpoint, after_span = snapshot(cli_path, server_path, ports, args.workers)
            wait_full_mesh(cli_path, ports, args.convergence_timeout, args.workers)
            elapsed = after_midpoint - before_midpoint
            deltas = {}
            for name in (*BYTE_FIELDS, *MESSAGE_FIELDS, *SHORT_MESSAGE_FIELDS, BUFFER_FIELD):
                value = sum(new[name] - old[name] for old, new in zip(before, after))
                if value < 0:
                    raise BenchmarkError(f"Counter {name} decreased during the {count}-node run")
                deltas[name] = value
            cpu_s = sum(new["cpu_s"] - old["cpu_s"] for old, new in zip(before, after))
            scopes = {row["cpu_scope"] for row in before + after}
            if len(scopes) != 1:
                raise BenchmarkError(f"Mixed CPU counter scopes: {sorted(scopes)}")
            return {
                "nodes": count,
                "elapsed_s": elapsed,
                "snapshot_span_s": {"before": before_span, "after": after_span},
                "known_nodes_min": min(row["known_nodes"] for row in after),
                "known_nodes_max": max(row["known_nodes"] for row in after),
                "full_mesh_connected": True,
                "delta": {**deltas, "cpu_s": cpu_s},
                "cpu_scope": scopes.pop(),
                "cpu_core_equivalents": cpu_s / elapsed,
                "bus_bytes_sent_per_node_per_s": deltas["cluster_stats_bytes_sent"] / count / elapsed,
                "bus_bytes_received_per_node_per_s": deltas["cluster_stats_bytes_received"] / count / elapsed,
                "ping_pong_sent_per_node_per_s": (
                    deltas["cluster_stats_messages_ping_sent"]
                    + deltas["cluster_stats_messages_pong_sent"]
                ) / count / elapsed,
                "short_messages_sent_per_node_per_s":
                    deltas["cluster_stats_bus_short_messages_sent"] / count / elapsed,
            }
        finally:
            stop_servers(processes)


def emit(payload, output):
    document = json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
    if output:
        Path(output).expanduser().write_text(document)
    sys.stdout.write(document)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", default=str(REPO / "src" / "redis-server"), help="redis-server binary")
    parser.add_argument("--cli", default=str(REPO / "src" / "redis-cli"), help="redis-cli binary")
    parser.add_argument("--nodes", type=int, nargs="+", default=[6, 40], metavar="N", help="cluster sizes (e.g. 6 40 100; 900 requires a provisioned host)")
    parser.add_argument("--duration", type=float, default=15.0, help="idle sample duration in seconds")
    parser.add_argument("--timeout-ms", type=int, default=30000, help="cluster-node-timeout in milliseconds")
    parser.add_argument("--base-port", type=int, default=19000, help="first client port; bus ports use +10000")
    parser.add_argument("--startup-timeout", type=float, default=60.0, help="seconds to wait for servers")
    parser.add_argument("--convergence-timeout", type=float, default=240.0, help="seconds to wait for all known nodes")
    parser.add_argument("--warmup", type=float, default=30.0, help="seconds after cluster convergence")
    parser.add_argument("--meet-timeout", type=float, default=30.0, help="redis-cli timeout for each CLUSTER MEET in seconds")
    parser.add_argument("--meet-retries", type=int, default=2, help="retries when a CLUSTER MEET command times out")
    parser.add_argument("--meet-delay", type=float, default=0.05, help="seconds between CLUSTER MEET commands")
    parser.add_argument("--workers", type=int, default=16, help="parallel redis-cli samplers")
    parser.add_argument("--output", help="also write JSON to this path")
    args = parser.parse_args()
    if (not args.nodes or any(n < 2 for n in args.nodes) or len(set(args.nodes)) != len(args.nodes)
            or args.duration <= 0 or args.timeout_ms <= 0 or args.startup_timeout <= 0
            or args.convergence_timeout <= 0 or args.warmup < 0 or args.meet_timeout <= 0
            or args.meet_retries < 0 or args.meet_delay < 0
            or args.workers < 1 or args.base_port < 1 or max(args.nodes) > 10000
            or args.base_port + max(args.nodes) - 1 + 10000 > 65535):
        parser.error("invalid cluster size, duration, timeout, worker count, or port range")
    return args


def handle_terminate(_signum, _frame):
    raise KeyboardInterrupt


def main():
    args = parse_args()
    signal.signal(signal.SIGTERM, handle_terminate)
    payload = {"schema_version": 1, "status": "ok", "runs": []}
    try:
        server_path = executable(args.server, "redis-server")
        cli_path = executable(args.cli, "redis-cli")
        payload.update(
            server=str(server_path), server_sha256=sha256(server_path),
            cli=str(cli_path), cli_sha256=sha256(cli_path),
            nodes=args.nodes, duration_s=args.duration, warmup_s=args.warmup,
            timeout_ms=args.timeout_ms, startup_timeout_s=args.startup_timeout,
            convergence_timeout_s=args.convergence_timeout,
            meet_timeout_s=args.meet_timeout, meet_retries=args.meet_retries,
            meet_delay_s=args.meet_delay, workers=args.workers,
            base_port=args.base_port, slots_assigned=False,
            resource_estimates=[{
                "nodes": n,
                "full_mesh_tcp_connections": n * (n - 1),
                "socket_endpoints_total": 2 * n * (n - 1),
                "bus_socket_endpoints_per_node": 2 * (n - 1),
            } for n in args.nodes],
            measurement_scope="Redis Cluster bus payload bytes; excludes TCP and TLS framing",
        )
        for count in args.nodes:
            print(f"Benchmarking {count} nodes...", file=sys.stderr, flush=True)
            payload["runs"].append(run_cluster(args, server_path, cli_path, count))
    except KeyboardInterrupt:
        payload.update(status="interrupted", error="Benchmark interrupted; all started Redis servers were stopped.")
        emit(payload, args.output)
        return 130
    except MissingMetrics as exc:
        payload.update(status="missing_metrics", error=str(exc))
        emit(payload, args.output)
        return 2
    except (BenchmarkError, OSError, KeyError, ValueError) as exc:
        payload.update(status="error", error=str(exc))
        emit(payload, args.output)
        return 1
    emit(payload, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
