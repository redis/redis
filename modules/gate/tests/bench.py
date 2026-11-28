#!/usr/bin/env python3
"""Rough performance numbers for the gate data type.

    python3 modules/gate/tests/bench.py

Starts a throwaway primary and replica from ./src/redis-server with
./modules/gate/gate.so, runs a few workloads and prints, per workload:

  * server-side CPU time per command (from INFO commandstats),
  * round trips per second of one Python client (mostly client overhead),
  * bytes sent to the replica per command,

then pipelined redis-benchmark throughput and the memory of typical gates.
Needs redis-py. Numbers are for orientation, not a rigorous benchmark.
"""

import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

import redis

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SERVER = os.path.join(ROOT, "src", "redis-server")
BENCH = os.path.join(ROOT, "src", "redis-benchmark")
MODULE = os.path.join(ROOT, "modules", "gate", "gate.so")
PORT = int(os.environ.get("GATE_BENCH_PORT", "21900"))

WORDS = ("thanks ok great deploy error report ticket build dashboard invoice "
         "query release notes please help explain review where who why how "
         "team everyone tomorrow perfect cool customer refund login password "
         "timeout latency cluster replica shard index vector search").split()


def start(port, workdir, *extra):
    os.makedirs(workdir, exist_ok=True)
    subprocess.check_call([SERVER, "--port", str(port), "--loadmodule", MODULE,
                           "--save", "", "--daemonize", "yes", "--dir", workdir,
                           "--logfile", os.path.join(workdir, "log"),
                           "--enable-debug-command", "local", *extra],
                          stdout=subprocess.DEVNULL)
    r = redis.Redis(port=port)
    for _ in range(100):
        try:
            r.ping()
            return r
        except redis.RedisError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def text(rng, nbytes):
    out = []
    size = 0
    while size < nbytes:
        w = rng.choice(WORDS)
        out.append(w)
        size += len(w) + 1
    return " ".join(out)[:nbytes]


def label_of(t):
    return "respond" if ("please" in t or "help" in t or "why" in t or "how" in t) else "ignore"


class Meter:
    """Per-command server CPU time and replication bytes around a workload."""

    def __init__(self, r):
        self.r = r

    def __enter__(self):
        self.r.config_resetstat()
        self.offset = self.r.info("replication")["master_repl_offset"]
        self.t0 = time.time()
        return self

    def __exit__(self, *exc):
        self.seconds = time.time() - self.t0
        stats = self.r.info("commandstats")
        self.cmds = {k[len("cmdstat_"):].lower(): v for k, v in stats.items()
                     if k.lower().startswith("cmdstat_g")}
        self.bytes = self.r.info("replication")["master_repl_offset"] - self.offset

    def show(self, name, ops):
        per = ", ".join(f"{c} {v['usec_per_call']:.1f} us" for c, v in sorted(self.cmds.items()))
        calls = sum(v["calls"] for v in self.cmds.values())
        print(f"{name:<44} {per}")
        print(f"{'':<44} {ops / self.seconds:,.0f} requests/s from one client, "
              f"{self.bytes / max(calls, 1):.0f} replicated bytes/command")


def train_classifier(r, key, rng, nbytes, classes=("ignore", "respond"), extra=()):
    """Run the client loop until the gate answers; returns requests used."""
    r.execute_command("GCREATE", key, "ALPHA", 0.05, "CLASSES", len(classes), *classes, *extra)
    for i in range(1, 200001):
        t = text(rng, nbytes)
        action, arg = r.execute_command("GPREDICT", key, "TEXT", t)
        if action == b"ACCEPT":
            return i
        if arg is not None:
            r.execute_command("GLEARN", key, arg, "LABEL", label_of(t))
    raise RuntimeError("the classifier gate never became ACTIVE")


def main():
    rng = random.Random(1)
    tmp = tempfile.mkdtemp(prefix="gate-bench-")
    r = start(PORT, os.path.join(tmp, "primary"))
    rep = start(PORT + 1, os.path.join(tmp, "replica"), "--replicaof", f"127.0.0.1 {PORT}")
    try:
        while rep.info("replication").get("master_link_status") != "up":
            time.sleep(0.05)
        n = 20000

        # -- a gate while it is still learning: every request is two commands
        with Meter(r) as m:
            used = train_classifier(r, "bench:warm", rng, 60)
        m.show(f"classifier, learning (ACTIVE after {used})", used)

        r.execute_command("GCREATE", "bench:score0", "ALPHA", 0.05, "SCORE")
        with Meter(r) as m:
            for _ in range(n):
                s = rng.random()
                action, arg = r.execute_command("GPREDICT", "bench:score0", "SCORE", s)
                if arg is not None and action != b"ACCEPT":
                    r.execute_command("GLEARN", "bench:score0", arg, "CORRECT",
                                      int(s > 0.6 or rng.random() < s))
        m.show("score gate, whole life (20,000 requests)", n)

        # -- gates that answer: AUDIT is tiny so that nearly every request is
        #    an ACCEPT and unanswered spot checks do not pile up
        tiny = ("AUDIT", 0.0005)
        for nbytes in (60, 600, 4096):
            key = f"bench:clf{nbytes}"
            train_classifier(r, key, rng, nbytes, extra=tiny)
            msgs = [text(rng, nbytes) for _ in range(500)]
            with Meter(r) as m:
                for i in range(n):
                    r.execute_command("GPREDICT", key, "TEXT", msgs[i % len(msgs)])
            m.show(f"classifier ACTIVE, {nbytes}-byte text", n)

        r.execute_command("GCREATE", "bench:score", "ALPHA", 0.05, "SCORE", *tiny)
        while True:
            action, arg = r.execute_command("GPREDICT", "bench:score", "SCORE", 0.99)
            if action == b"ACCEPT":
                break
            if arg is not None:
                r.execute_command("GLEARN", "bench:score", arg, "CORRECT", 1)
        with Meter(r) as m:
            for _ in range(n):
                r.execute_command("GPREDICT", "bench:score", "SCORE", 0.99)
        m.show("score gate ACTIVE", n)

        with Meter(r) as m:
            for _ in range(n):
                r.execute_command("GINFO", "bench:score")
        m.show("GINFO", n)

        # -- redis-benchmark: 50 connections, with and without pipelining
        print("\nredis-benchmark, 50 clients (requests/s pipelined x16; latency unpipelined):")
        for name, cmd in (("GPREDICT score (ACCEPT)", ["GPREDICT", "bench:score", "SCORE", "0.99"]),
                          ("GPREDICT 60-byte text (ACCEPT)",
                           ["GPREDICT", "bench:clf60", "TEXT", text(rng, 60)]),
                          ("GPREDICT 600-byte text (ACCEPT)",
                           ["GPREDICT", "bench:clf600", "TEXT", text(rng, 600)]),
                          ("GINFO", ["GINFO", "bench:score"])):
            def run(pipeline, n):
                out = subprocess.run([BENCH, "-p", str(PORT), "--csv", "-n", str(n), "-c", "50",
                                      "-P", str(pipeline), *cmd], capture_output=True, text=True).stdout
                return out.strip().splitlines()[-1].replace('"', "").split(",")[-7:]
            fast = run(16, 400000)
            slow = run(1, 100000)
            print(f"  {name:<32} {float(fast[0]):>10,.0f}/s   p50 {slow[3]} ms  p99 {slow[5]} ms")

        # -- memory (gates whose every request was answered, then one that waits)
        print("\nmemory (MEMORY USAGE):")
        r.execute_command("GCREATE", "m:score", "ALPHA", 0.05, "SCORE")
        r.execute_command("GCREATE", "m:c2", "ALPHA", 0.05, "CLASSES", 2, "a", "b")
        r.execute_command("GCREATE", "m:c8", "ALPHA", 0.05, "CLASSES", 8, *"abcdefgh")
        r.execute_command("GCREATE", "m:c64", "ALPHA", 0.05, "CLASSES", 64,
                          *[f"l{i}" for i in range(64)])
        for key, what in (("m:score", "score gate, new"), ("bench:score0", "score gate, ACTIVE"),
                          ("m:c2", "classifier, 2 labels, new"),
                          ("bench:warm", "classifier, 2 labels, ACTIVE"),
                          ("m:c8", "classifier, 8 labels, new"),
                          ("m:c64", "classifier, 64 labels, new")):
            print(f"  {what:<34} {r.memory_usage(key):>10,} bytes")
        for i in range(3000):
            r.execute_command("GPREDICT", "m:c2", "TEXT", text(rng, 200))
        print(f"  {'... 3,000 unanswered 200-byte requests':<34} {r.memory_usage('m:c2'):>10,} bytes")

        ok = all(r.execute_command("DEBUG", "DIGEST-VALUE", k) ==
                 rep.execute_command("DEBUG", "DIGEST-VALUE", k)
                 for k in r.keys("*")) if wait_synced(r, rep) else False
        print(f"\nreplica identical to primary after all of the above: {'yes' if ok else 'NO'}")
    finally:
        for c in (rep, r):
            try:
                c.execute_command("SHUTDOWN", "NOSAVE")
            except redis.RedisError:
                pass
        shutil.rmtree(tmp, ignore_errors=True)


def wait_synced(r, rep):
    for _ in range(200):
        if (rep.info("replication").get("master_repl_offset") ==
                r.info("replication")["master_repl_offset"]):
            return True
        time.sleep(0.05)
    return False


if __name__ == "__main__":
    sys.exit(main())
