#!/usr/bin/env python3
"""A reproducible demonstration of both kinds of gate.

Start a server with the module, then run this script:

    ./src/redis-server --loadmodule ./modules/gate/gate.so --port 6379
    python3 modules/gate/examples/demo.py [--port 6379] [--requests 20000]

The "teacher" here is a deterministic rule, standing in for an LLM call.
The script counts how many teacher calls the gate saved and how often an
accepted answer disagreed with what the teacher would have said.
"""

import argparse
import random

import redis

from gate_client import Gate

IGNORE = ["thanks", "thank you", "ok", "great", "got it", "sounds good", "lol",
          "nice", "perfect", "cool", "will do", "see you tomorrow", "bye"]
RESPOND = ["can you explain", "please help me with", "how do i", "what is",
           "could you look at", "why does", "any idea about", "please review",
           "where can i find", "who owns"]
TOPICS = ["the deploy", "this error", "the report", "that ticket", "the build",
          "our dashboard", "the invoice", "this query", "the release notes"]


def make_message(rng):
    """A chat message and the label the teacher gives it."""
    if rng.random() < 0.65:
        text = rng.choice(IGNORE) + rng.choice(["", "!", " :)", " everyone", " team"])
        label = "ignore"
    else:
        text = rng.choice(RESPOND) + " " + rng.choice(TOPICS) + "?"
        label = "respond"
    if rng.random() < 0.02:               # the teacher is not perfectly predictable
        label = "respond" if label == "ignore" else "ignore"
    return text, label


def report(name, gate, n, saved, wrong):
    info = gate.info()
    print(f"\n{name}")
    print(f"  requests            {n}")
    print(f"  teacher calls saved {saved} ({100.0 * saved / n:.1f}%)")
    print(f"  wrong among saved   {wrong} ({100.0 * wrong / max(saved, 1):.2f}%)"
          f"   target alpha {info['alpha']}")
    print(f"  state {info['state']}, generation {info['generation']}, "
          f"thresholds {info['thresholds']}, coverage now {float(info['coverage']):.2f}")
    print(f"  spot checks {info['audit-samples']} with {info['audit-errors']} disagreements")


def classifier_demo(r, n, rng):
    gate = Gate(r, "demo:respond")
    r.delete(gate.key)
    gate.create(alpha=0.05, classes=["ignore", "respond"])
    saved = wrong = 0
    first = None
    for i in range(n):
        text, truth = make_message(rng)
        calls = []
        label = gate.decide(text, lambda t: calls.append(1) or truth)
        if not calls:
            saved += 1
            wrong += label != truth
            if first is None:
                first = i + 1
    report("Classifier gate (should the agent respond?)", gate, n, saved, wrong)
    print(f"  first request answered by the gate: #{first}")


def score_demo(r, n, rng):
    gate = Gate(r, "demo:cache")
    r.delete(gate.key)
    gate.create(alpha=0.05)
    saved = wrong = 0
    first = None
    for i in range(n):
        score = rng.random()                       # e.g. a cache similarity
        cheap_ok = rng.random() < (0.995 if score > 0.8 else score)
        calls = []
        gate.decide_score(score, "cached", lambda: calls.append(1) or "fresh",
                          lambda cheap, real: cheap_ok)
        if not calls:
            saved += 1
            wrong += not cheap_ok
            if first is None:
                first = i + 1
    report("Score gate (is the cached answer good enough?)", gate, n, saved, wrong)
    print(f"  first request answered by the gate: #{first}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--requests", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--resp", type=int, default=3, choices=[2, 3])
    a = ap.parse_args()
    r = redis.Redis(host=a.host, port=a.port, protocol=a.resp)
    rng = random.Random(a.seed)
    classifier_demo(r, a.requests, rng)
    score_demo(r, a.requests, rng)


if __name__ == "__main__":
    main()
