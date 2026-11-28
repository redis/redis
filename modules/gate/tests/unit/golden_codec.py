#!/usr/bin/env python3
"""Reference encoder for the gate codec.

Written from the LAYOUT comment of gate_codec.c and the format rules of
gate_codec.h with struct.pack. It never runs the C code and shares nothing
with gateEncode(), so the three blobs it writes are an independent statement
of the format. tests/unit/test_codec.c builds the same three gates by hand:
gateEncode() must produce these bytes, gateDecode() of these bytes must
produce those gates.

    golden_codec.py                 print the blobs as C, ready for test_codec.c
    golden_codec.py --check FILE    exit 1 unless FILE holds exactly that text

The text form of a blob: hex bytes, with '*N' standing for N zero bytes.
Next to each blob the script prints where the fields that a digest zeroes
are, and the numbers that are not stored and that the decoder must compute
again (bars, thresholds), derived here from the rules of gate_core.h.
"""
import math
import struct
import sys

DIM = 8192          # GATE_DIM
T = 32              # GATE_THRESHOLDS

CODEC_VERSION = 1
FEATURES_VERSION = 1

KIND_CLASSIFIER, KIND_SCORE = 0, 1

# Request flags (GATE_OBS_* in gate_core.h).
AUDIT, WOULD, CAND, SERV, RESOLVED, PROVISIONAL = 1, 2, 4, 8, 16, 32


def u8(v):  return struct.pack('<B', v)
def u16(v): return struct.pack('<H', v)
def u32(v): return struct.pack('<I', v)
def i32(v): return struct.pack('<i', v)
def u64(v): return struct.pack('<Q', v)
def i64(v): return struct.pack('<q', v)
def f64(v): return struct.pack('<d', v)
def f32(v): return struct.pack('<f', v)


def header():
    return b'GATE' + u16(CODEC_VERSION) + u16(FEATURES_VERSION)


def config(kind, alpha, delta, audit, labels, class_alpha, class_defer,
           snapshot_examples, timeout):
    assert len(labels) == len(class_alpha) == len(class_defer)
    out = bytearray(u8(kind) + f64(alpha) + f64(delta) + f64(audit))
    out += u32(len(labels))
    for l in labels:
        out += u32(len(l)) + l
    for a in class_alpha:
        out += f64(a)
    for d in class_defer:
        out += u8(d)
    out += u32(snapshot_examples) + i64(timeout)
    return bytes(out)


def ledger(lineage, epoch, created, now, generations, paused, snapshot_updates,
           blind, missing_run):
    return (u64(lineage) + u64(epoch) + i64(created) + i64(now) + u64(generations) +
            u8(paused) + u64(snapshot_updates) + u8(blind) + u32(missing_run))


LEDGER_NOW = 24     # offset of now_ms inside the ledger


def model(k, updates, w, b):
    """w: {index: value} over V * DIM weights, b: list of V biases."""
    v = 1 if k == 2 else k
    arr = [0.0] * (v * DIM)
    for i, x in w.items():
        arr[i] = x
    assert len(b) == v
    return u64(updates) + struct.pack('<%df' % (v * DIM), *arr) + struct.pack('<%df' % v, *b)


def region(logw=(0.0, 0.0, 0.0, 0.0), n=0, errors=0, certified_at=0):
    return b''.join(f64(x) for x in logw) + u64(n) + u64(errors) + u64(certified_at)


def drift(logw=(0.0, 0.0), n=0, errors=0):
    return b''.join(f64(x) for x in logw) + u64(n) + u64(errors)


def absent():
    return u8(0)


def generation(k, rows, gen, frozen, promoted, samples, errors, paired, audits,
               audit_errors, budget, mdl, regions, drifts):
    """regions: {row * T + t: region bytes}; drifts: {row: drift bytes}."""
    out = bytearray(u8(1))
    out += u64(gen) + i64(frozen) + i64(promoted) + u64(samples) + u64(errors)
    out += u64(paired) + u64(audits) + u64(audit_errors) + u64(budget)
    if k:
        out += mdl
    for i in range(rows * T):
        out += regions.get(i, region())
    for r in range(rows):
        out += drifts.get(r, drift())
    return bytes(out)


def request(seq, created, flags, cand_label, serv_label, cand_score, serv_score,
            cand_gen, serv_gen, result, idx=(), val=()):
    assert len(idx) == len(val)
    out = bytearray()
    out += u64(seq) + i64(created) + u32(flags) + i32(cand_label) + i32(serv_label)
    out += f64(cand_score) + f64(serv_score) + u64(cand_gen) + u64(serv_gen)
    out += i32(result) + u32(len(idx))
    for i in idx:
        out += u32(i)
    for v in val:
        out += f32(v)
    return bytes(out)


def requests(next_seq, reqs):
    return u64(next_seq) + u32(len(reqs)) + b''.join(reqs)


def counters(c):
    out = bytearray()
    for name in ('requests', 'accepted', 'audited', 'abstained', 'full', 'window'):
        out += u64(c[name])
    out += f64(c['coverage'])
    for name in ('observations', 'learned', 'ignored', 'missing', 'dropped',
                 'promotions', 'revocations', 'retired'):
        out += u64(c[name])
    return bytes(out)


class Blob:
    """The sections of a blob in order, and where two of them start."""

    def __init__(self):
        self.parts = []
        self.now = self.counters = None

    def add(self, part):
        self.parts.append(part)

    def size(self):
        return sum(map(len, self.parts))

    def add_ledger(self, part):
        self.now = self.size() + LEDGER_NOW
        self.add(part)

    def add_counters(self, part):
        self.counters = self.size()
        self.add(part)

    def bytes(self):
        return b''.join(self.parts)


# ---------------------------------------------------------------------------
# The score gate: it answers (generation 3 serves, its widest region is
# proven) and nothing is being tested. Three spot checks wait: one the proof
# has moved on without (PROVISIONAL), one for its answer, and one behind
# that with its answer.

def score_gate():
    b = Blob()
    b.add(header())
    b.add(config(KIND_SCORE, 0.2, 0.01, 1.0, [], [], [], 1000, 5000))
    b.add_ledger(ledger(0x0102030405060708, 0x1112131415161718, -2, 1700000000000, 3,
                        0, 0, 0, 7))
    b.add(absent())                           # no trainer
    b.add(absent())                           # no candidate
    regions = {
        0: region((0.5, 1.0, 1.5, 2.0), 50, 2, 41),
        1: region((-0.25, 0.25, 0.75, 1.25), 30, 0, 0),
        31: region((0.0, 0.0, 0.0, -3.0), 0, 0, 0),
    }
    b.add(generation(0, 1, 3, 1699999990000, 1699999995000, 50, 2, 0, 6, 1, 0,
                     None, regions, {0: drift((0.125, -0.375), 6, 1)}))
    b.add(requests(9, [
        request(5, 1699999998000, AUDIT | SERV | PROVISIONAL, -1, -1, 0.0, 0.75, 0, 3, -1),
        request(7, 1699999999000, AUDIT | SERV, -1, -1, 0.0, 0.625, 0, 3, -1),
        request(8, 1699999999500, AUDIT | WOULD | SERV | RESOLVED, -1, -1, 0.0, 0.03125,
                0, 3, 1),
    ]))
    b.add_counters(counters(dict(requests=100, accepted=60, audited=10, abstained=30,
                                 full=4, window=100, coverage=0.75, observations=40,
                                 learned=30, ignored=2, missing=1, dropped=3,
                                 promotions=1, revocations=2, retired=0)))
    return b


# ---------------------------------------------------------------------------
# The classifier: three labels (one with a stricter error rate, one
# deferred), paused, generation 5 serving and generation 7 being tested, and
# five waiting requests: one kept for training from before any snapshot, one
# the proof has moved on without, an unanswered piece of evidence, an
# answered one behind it, and one recorded while paused.

def classifier_gate():
    k = 3
    labels = [b'spam', b'ham', 'héllo ✓'.encode('utf-8')]
    b = Blob()
    b.add(header())
    b.add(config(KIND_CLASSIFIER, 0.1, 0.0625, 0.25, labels, [0.1, 0.05, 0.1], [0, 0, 1],
                 20, 300000))
    b.add_ledger(ledger(0x1122334455667788, 0x99aabbccddeeff00, 1700000000000,
                        1700000123456, 7, 1, 40, 0, 31))
    b.add(u8(1) + model(k, 45, {0: 0.5, 8191: -0.25, 8192: 1.5, 24575: 64.0},
                        [0.125, -64.0, 2.0]))
    cand_regions = {
        0: region((0.1, 0.2, 0.3, 0.4), 12, 3, 0),
        1: region((1.5, 2.5, 3.5, 4.5), 9, 1, 7),
        32: region(n=2),
    }
    b.add(generation(k, 3, 7, 1700000020000, 0, 12, 3, 10, 0, 0, 560,
                     model(k, 40, {1: -1.0, 16384: 0.75}, [0.0, 0.0, -0.5]),
                     cand_regions, {}))
    serv_regions = {0: region((-0.1, -0.2, -0.3, -0.4), 10, 1, 0)}
    for t in range(8):                        # row 1, thresholds 0..7
        serv_regions[32 + t] = region(n=80 - t)
    serv_regions[32 + 0] = region(n=80, errors=3)
    serv_regions[32 + 4] = region(n=76, certified_at=50)
    serv_regions[32 + 6] = region(n=74, certified_at=70)
    serv_regions[32 + 7] = region((9.0, 9.5, 10.0, 10.5), 73, 0, 61)
    b.add(generation(k, 3, 5, 1700000005000, 1700000010000, 90, 4, 0, 11, 2, 0,
                     model(k, 20, {8191: 2.0}, [1.0, 0.0, 0.0]),
                     serv_regions, {1: drift((1.25, -0.5), 11, 2)}))
    b.add(requests(11, [
        request(2, 1700000001000, 0, -1, -1, 0.0, 0.0, 0, 0, -1, [3, 4000], [1.0, -1.0]),
        request(3, 1700000040000, AUDIT | CAND | PROVISIONAL, 0, 2, 0.625, 0.53125, 7, 5, -1,
                [7, 8190], [0.5, 1.5]),
        request(5, 1700000110000, AUDIT | CAND | SERV, 1, 0, 0.75, 0.5, 7, 5, -1,
                [5, 100, 8191], [1.0, -0.5, 0.25]),
        request(6, 1700000110001, AUDIT | WOULD | CAND | SERV | RESOLVED, 0, 1, 0.875, 0.9375,
                7, 5, 2),
        request(9, 1700000123456, WOULD, 2, 1, 0.4375, 0.96875, 7, 5, -1, [0], [-2.0]),
    ]))
    b.add_counters(counters(dict(requests=1000, accepted=700, audited=100, abstained=200,
                                 full=7, window=512, coverage=0.5, observations=300,
                                 learned=250, ignored=4, missing=5, dropped=6,
                                 promotions=2, revocations=1, retired=3)))
    return b


# ---------------------------------------------------------------------------
# The blind gate: a score gate whose answers stopped arriving. It holds no
# generation. Three requests wait: one the proof had moved on without before
# the generation it was proof for was dropped, and two recorded while blind
# so that there is an id to answer (one of them drew an audit).

def blind_gate():
    b = Blob()
    b.add(header())
    b.add(config(KIND_SCORE, 0.05, 0.1, 0.5, [], [], [], 7, 60000))
    b.add_ledger(ledger(0xa1a2a3a4a5a6a7a8, 0xb1b2b3b4b5b6b7b8, 1, 86400000, 4, 0, 0, 1, 0))
    b.add(absent())                           # no trainer
    b.add(absent())                           # no candidate
    b.add(absent())                           # nothing serving
    b.add(requests(0x123456789, [
        request(0x123456000, 86350000, CAND | PROVISIONAL, -1, -1, 0.9375, 0.0, 4, 0, -1),
        request(0x123456700, 86399000, AUDIT, -1, -1, 0.0, 0.0, 0, 0, -1),
        request(0x123456788, 86400000, 0, -1, -1, 0.0, 0.0, 0, 0, -1),
    ]))
    b.add_counters(counters(dict(requests=900, accepted=100, audited=50, abstained=750,
                                 full=0, window=0, coverage=0.0, observations=700,
                                 learned=500, ignored=0, missing=96, dropped=12,
                                 promotions=3, revocations=3, retired=0)))
    return b


# ---------------------------------------------------------------------------
# What is not stored (gate_core.h, PROVEN and the threshold grid of
# gate_stats.h).

def bar(g, tested_rows, delta):
    """log(g (g + 1)) + log(tested rows * GATE_THRESHOLDS) - log(delta)"""
    return math.log(g * (g + 1)) + math.log(tested_rows * T) - math.log(delta)


def grid(lo, hi, t):
    """1 - (1 - lo) * ((1 - hi) / (1 - lo)) ^ (t / (T - 1))"""
    return 1.0 - (1.0 - lo) * ((1.0 - hi) / (1.0 - lo)) ** (t / (T - 1.0))


# ---------------------------------------------------------------------------
# Text form.

def rle(blob):
    """Hex text; a run of 8 or more zero bytes becomes '*N'."""
    toks, i, n = [], 0, len(blob)
    while i < n:
        j = i
        while j < n and blob[j] == 0:
            j += 1
        if j - i >= 8:
            toks.append('*%d' % (j - i))
            i = j
            continue
        j = i
        zeros = 0
        while j < n:
            zeros = zeros + 1 if blob[j] == 0 else 0
            j += 1
            if zeros == 8:
                j -= 8
                break
        toks.append(blob[i:j].hex())
        i = j
    return toks


def unrle(toks):
    out = bytearray()
    for t in toks:
        out += bytes(int(t[1:])) if t[0] == '*' else bytes.fromhex(t)
    return bytes(out)


def emit(name, blob):
    data = blob.bytes()
    toks = rle(data)
    assert unrle(toks) == data
    up = name.upper()
    lines = ['#define GOLDEN_%s_BYTES %d' % (up, len(data)),
             '#define GOLDEN_%s_NOW %d' % (up, blob.now),
             '#define GOLDEN_%s_COUNTERS %d' % (up, blob.counters),
             '',
             'static const char golden_%s_text[] =' % name]
    text, line = [], ''
    for t in toks:
        while len(t) > 64:                    # long hex tokens: split
            if line:
                text.append(line)
                line = ''
            text.append(t[:64])
            t = t[64:]
        if len(line) + len(t) + 1 > 66:
            text.append(line)
            line = ''
        line = (line + ' ' + t) if line else t
    if line:
        text.append(line)
    for i, l in enumerate(text):
        lines.append('    "%s%s' % (l, '";' if i == len(text) - 1 else ' "'))
    return lines


def generated():
    out = ['/* Written by tests/unit/golden_codec.py: do not edit, run it again. */', '']
    out += emit('score', score_gate()) + ['']
    out += emit('classifier', classifier_gate()) + ['']
    out += emit('blind', blind_gate()) + ['']
    out += [
        '/* What the blobs do not store. Score gate: one tested row, generation 3,',
        ' * DELTA 0.01; it answers from its lowest threshold, grid[0] = 0. Classifier:',
        ' * two tested rows, generations 7 (tested) and 5 (answering), DELTA 0.0625;',
        ' * row 1 answers from grid[4], with the grid running from 1/3 to 0.999. */',
        '#define GOLDEN_SCORE_BAR %.17g' % bar(3, 1, 0.01),
        '#define GOLDEN_CLASSIFIER_CANDIDATE_BAR %.17g' % bar(7, 2, 0.0625),
        '#define GOLDEN_CLASSIFIER_SERVING_BAR %.17g' % bar(5, 2, 0.0625),
        '#define GOLDEN_CLASSIFIER_THRESHOLD %.17g' % grid(1.0 / 3.0, 0.999, 4),
        '',
        '/* End of what golden_codec.py writes. */',
    ]
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    text = generated()
    if len(sys.argv) == 3 and sys.argv[1] == '--check':
        with open(sys.argv[2]) as f:
            if text not in f.read():
                sys.exit('%s does not hold what golden_codec.py writes' % sys.argv[2])
        print('%s: the reference blobs are up to date' % sys.argv[2])
    elif len(sys.argv) == 1:
        sys.stdout.write(text)
    else:
        sys.exit(__doc__)
