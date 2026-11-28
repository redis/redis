#!/usr/bin/env python3
"""Minimal client for the Redis gate data type.

    gate = Gate(redis.Redis(), "agent:respond")
    gate.create(alpha=0.02, classes=["ignore", "respond"])
    label = gate.decide(text, teacher)        # asks the teacher only when needed

Works with RESP2 and RESP3 connections. See ../SPEC.md for the commands.
"""

import redis


def _s(v):
    return v.decode() if isinstance(v, bytes) else v


def _as_dict(reply):
    """GINFO is a map in RESP3 and a flat field/value array in RESP2."""
    if isinstance(reply, dict):
        items = reply.items()
    else:
        items = zip(reply[::2], reply[1::2])
    out = {}
    for k, v in items:
        if isinstance(v, list):
            v = [_s(x) for x in v]
        out[_s(k)] = _s(v)
    return out


class Gate:
    def __init__(self, client, key):
        self.r = client
        self.key = key

    # -- setup ---------------------------------------------------------------

    def create(self, alpha, classes=None, audit=None, delta=None,
               always_defer=None, class_alpha=None):
        """A classifier gate if 'classes' is given, otherwise a score gate."""
        args = ["GCREATE", self.key, "ALPHA", alpha]
        if classes:
            args += ["CLASSES", len(classes), *classes]
        else:
            args += ["SCORE"]
        if audit is not None:
            args += ["AUDIT", audit]
        if delta is not None:
            args += ["DELTA", delta]
        if always_defer:
            args += ["ALWAYSDEFER", len(always_defer), *always_defer]
        if class_alpha:
            args += ["CLASSALPHA", len(class_alpha)]
            for label, a in class_alpha.items():
                args += [label, a]
        return self.r.execute_command(*args)

    # -- the three commands of the hot path ----------------------------------

    def predict(self, text=None, features=None, score=None):
        """Returns (action, argument): ("ACCEPT", label), ("ABSTAIN", id or
        None) or ("AUDIT", id)."""
        if score is not None:
            reply = self.r.execute_command("GPREDICT", self.key, "SCORE", score)
        else:
            args = ["GPREDICT", self.key, "TEXT", text]
            if features:
                args += ["FEATURES", len(features)]
                for name, value in features.items():
                    args += [name, value]
            reply = self.r.execute_command(*args)
        return _s(reply[0]), _s(reply[1])

    def learn(self, observation_id, label=None, correct=None):
        """Tell the gate what the teacher said. Safe to retry."""
        if label is not None:
            return self.r.execute_command("GLEARN", self.key, observation_id, "LABEL", label)
        return self.r.execute_command("GLEARN", self.key, observation_id,
                                      "CORRECT", 1 if correct else 0)

    def add(self, text, label, features=None):
        """Teach one labelled example directly (history, bootstrapping)."""
        args = ["GADD", self.key, "TEXT", text]
        if features:
            args += ["FEATURES", len(features)]
            for name, value in features.items():
                args += [name, value]
        return self.r.execute_command(*args, "LABEL", label)

    # -- the rest -------------------------------------------------------------

    def info(self):
        return _as_dict(self.r.execute_command("GINFO", self.key))

    def pause(self):
        return self.r.execute_command("GPAUSE", self.key)

    def resume(self):
        return self.r.execute_command("GRESUME", self.key)

    def reset(self):
        return self.r.execute_command("GRESET", self.key)

    # -- the whole client contract in one place --------------------------------

    def decide(self, text, teacher, features=None):
        """Return a label for 'text', calling teacher(text) only when the
        gate says so. If Redis fails in any way, the teacher decides."""
        try:
            action, arg = self.predict(text=text, features=features)
        except redis.RedisError:
            return teacher(text)
        if action == "ACCEPT":
            return arg
        label = teacher(text)              # ABSTAIN and AUDIT
        if arg is not None:
            try:
                self.learn(arg, label=label)
            except redis.RedisError:
                pass                       # the gate counts it as missing
        return label

    def decide_score(self, score, cheap_result, teacher, same):
        """Score gate: return cheap_result when the gate accepts, otherwise
        the teacher's result. 'same(cheap, real)' judges whether the cheap
        result would have been acceptable; it only runs when the gate asks."""
        try:
            action, arg = self.predict(score=score)
        except redis.RedisError:
            return teacher()
        if action == "ACCEPT":
            return cheap_result
        real = teacher()
        if arg is not None:
            try:
                self.learn(arg, correct=same(cheap_result, real))
            except redis.RedisError:
                pass
        return real
