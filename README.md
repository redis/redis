# DS614 Big Data Engineering — Final Project Report
## Redis HyperLogLog: A Probabilistic Cardinality Estimation System

**Course:** DS614 — Big Data Engineering  
**System Studied:** Redis 7.2 — HyperLogLog (`src/hyperloglog.c`)  
**Group Member:** 202518023 and 202518036

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Execution Trace — PFADD → PFCOUNT](#2-execution-trace--pfadd--pfcount)
3. [Design Decisions](#3-design-decisions)
4. [Concept Mapping](#4-concept-mapping)
5. [Experiments & Results](#5-experiments--results)
6. [Failure Analysis](#6-failure-analysis)
7. [Key Insights](#7-key-insights)
8. [GitHub Artifacts](#8-github-artifacts)

---

## 1. System Overview

### What Problem Does HyperLogLog Solve?

Counting unique elements (cardinality) in large-scale data streams is a fundamental problem in data engineering. Exact counting requires storing every unique element ever seen — for 100 million unique users this means gigabytes of memory **per counter**. At scale, this becomes completely impractical.

Redis HyperLogLog solves this by answering:

> *"Approximately how many unique elements have been added?"*

with only **12KB of fixed memory** and a guaranteed **~0.81% error rate** — regardless of whether you have 100 or 100 million unique elements.

### What This System IS and IS NOT

| HyperLogLog IS | HyperLogLog IS NOT |
|---|---|
| A cardinality estimator | An exact counter |
| A streaming algorithm (single pass) | A storage system |
| Memory-constant (always 12KB dense) | Reversible (cannot retrieve elements) |
| Mergeable across distributed nodes | Suitable when exact count is required |

### Real-World Use Cases

- Count unique visitors to a website per day
- Count unique search queries per hour
- Count unique IPs hitting an API endpoint
- Count unique products viewed per user session
- Detect duplicate events in event streams

### How to Use It (Redis Commands)

```bash
# Add elements to HyperLogLog
PFADD myhll "apple" "banana" "cherry"
# (integer) 1  ← registers were updated

# Get estimated unique count
PFCOUNT myhll
# (integer) 3

# Duplicates are automatically ignored
PFADD myhll "apple"
PFCOUNT myhll
# (integer) 3  ← unchanged, idempotent

# Merge multiple HLLs (distributed use case)
PFMERGE result hll_region_a hll_region_b
PFCOUNT result
# returns union cardinality of both HLLs
```

### System Setup (Verified)

```bash
# Clone Redis 7.2 source
git clone https://github.com/redis/redis.git
cd redis && git checkout 7.2

# Compile from source
make

# Start server
src/redis-server

# Connect client
src/redis-cli
```

Primary source file: `src/hyperloglog.c` (~1,300 lines)

---

## 2. Execution Trace — PFADD → PFCOUNT

This section traces one complete execution path through the system, referencing actual source code functions and line numbers.

### Key Function Map

| Function | File | Line | Role |
|---|---|---|---|
| `pfaddCommand` | hyperloglog.c | 1229 | PFADD Redis command entry point |
| `hllAdd` | hyperloglog.c | 1088 | Routes to sparse or dense path |
| `hllPatLen` | hyperloglog.c | 452 | Hashes element → register index + count |
| `MurmurHash64A` | hyperloglog.c | 397 | Produces uniform 64-bit hash |
| `hllDenseSet` | hyperloglog.c | ~514 | Updates register if new count is higher |
| `hllSparseToDense` | hyperloglog.c | 586 | One-way sparse→dense promotion |
| `pfcountCommand` | hyperloglog.c | 1269 | PFCOUNT Redis command entry point |
| `hllCount` | hyperloglog.c | 1050 | Applies HyperLogLog estimation formula |

---

### 2.1 PFADD Execution Path

**Entry Point:** `pfaddCommand()` — `src/hyperloglog.c` line 1229

```c
void pfaddCommand(client *c) {
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    // c->argv[0] = "PFADD"   (command)
    // c->argv[1] = "myhll"   (key name)
    // c->argv[2..n] = elements ("apple", "banana", ...)

    if (o == NULL) {
        o = createHLLObject();        // key doesn't exist → create fresh HLL
        dbAdd(c->db, c->argv[1], o); // save to Redis DB
        updated++;
    } else {
        isHLLObjectOrReply(c, o);     // validate it's really an HLL
        o = dbUnshareStringValue(...);// private copy for modification
    }

    for (j = 2; j < c->argc; j++) {
        int retval = hllAdd(o, c->argv[j]->ptr, sdslen(c->argv[j]->ptr));
        // retval = 1 → register updated (new unique element)
        // retval = 0 → no change (duplicate element)
        // retval = -1 → error (corrupted HLL)
        if (retval == 1) updated++;
    }

    if (updated) {
        signalModifiedKey(...);          // notify Redis internals
        notifyKeyspaceEvent(...,"pfadd");// publish keyspace event
        server.dirty += updated;         // mark for AOF/RDB persistence
        HLL_INVALIDATE_CACHE(hdr);       // clear cached cardinality
    }

    addReply(c, updated ? shared.cone : shared.czero);
    // Returns (integer) 1 if any register changed
    // Returns (integer) 0 if nothing changed
}
```

---

**Step 2:** `hllAdd()` — `src/hyperloglog.c` line 1088

```c
int hllAdd(robj *o, unsigned char *ele, size_t elesize) {
    struct hllhdr *hdr = o->ptr;
    switch(hdr->encoding) {
    case HLL_DENSE:  return hllDenseAdd(hdr->registers, ele, elesize);
    case HLL_SPARSE: return hllSparseAdd(o, ele, elesize);
    default:         return -1;   // corrupted / unknown encoding
    }
}
```

This is a pure routing function — it checks whether the HLL currently uses sparse or dense encoding and calls the appropriate path. This is where **Design Decision #2** (adaptive encoding) is enforced.

---

**Step 3:** `hllPatLen()` — `src/hyperloglog.c` line 452

This is the **mathematical core** of HyperLogLog. It converts any element into two values: which register to update, and what value to store.

```c
int hllPatLen(unsigned char *ele, size_t elesize, long *regp) {
    uint64_t hash, bit, index;
    int count;

    // Step 1: Hash the element to a uniform 64-bit number
    hash = MurmurHash64A(ele, elesize, 0xadc83b19ULL);

    // Step 2: Extract register index from first 14 bits
    index = hash & HLL_P_MASK;   // HLL_P_MASK = 0x3FFF (14-bit mask)
    // This selects 1 of 16,384 registers

    // Step 3: Shift away the 14 index bits
    hash >>= HLL_P;               // HLL_P = 14

    // Step 4: Safety termination bit
    hash |= ((uint64_t)1 << HLL_Q);
    // Forces a 1 at position 51 — guarantees loop terminates

    // Step 5: Count leading zeros + 1
    bit = 1;
    count = 1;
    while((hash & bit) == 0) {
        count++;
        bit <<= 1;
    }
    // count = 1  → hash starts with 1xxxxxxx (probability 1/2)
    // count = 2  → hash starts with 01xxxxxx (probability 1/4)
    // count = 10 → hash starts with 0000000001... (probability 1/1024)

    *regp = (int) index;
    return count;
}
```

**The Probability Insight:**

```
Leading zeros → Probability → Implication
count = 1     → 1/2         → very common,  few unique elements
count = 2     → 1/4         → common
count = 5     → 1/32        → less common
count = 10    → 1/1024      → rare,  ~1024 unique elements seen
count = 20    → 1/1048576   → very rare, ~1M unique elements seen
```

---

**Step 4:** `MurmurHash64A()` — `src/hyperloglog.c` line 397

```c
uint64_t MurmurHash64A(const void *key, size_t len, unsigned int seed) {
    const uint64_t m = 0xc6a4a7935bd1e995; // magic multiplier for bit mixing
    const int r = 47;                        // optimal shift for 64-bit

    uint64_t h = seed ^ (len * m);           // initialize with seed XOR length

    // Process 8 bytes at a time (fast!)
    while(data != end) {
        k *= m;
        k ^= k >> r;   // avalanche: shift then XOR
        k *= m;
        h ^= k;
        h *= m;
        data += 8;
    }

    // Handle remaining bytes (if input length not divisible by 8)
    switch(len & 7) {
    case 7: h ^= (uint64_t)data[6] << 48;
    // ... cases 6 through 1
    }

    // Final avalanche mixing
    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}
```

The function produces a **uniformly distributed** 64-bit output — every bit has equal 50/50 probability of being 0 or 1. This uniform distribution is what makes HyperLogLog's probability math work correctly.

---

### 2.2 Register Update

After `hllPatLen()` returns an index and count:

```c
int hllDenseSet(uint8_t *registers, long index, uint8_t count) {
    uint8_t oldcount;
    HLL_DENSE_GET_REGISTER(oldcount, registers, index);

    if (count > oldcount) {
        HLL_DENSE_SET_REGISTER(registers, index, count);
        return 1;   // register updated → PFADD returns 1
    } else {
        return 0;   // no change → duplicate detected
    }
}
```

**Why duplicates don't affect count:** The same element always hashes to the same index with the same count. Since `count > oldcount` is never true for duplicates, the register is never updated. This is the source of HyperLogLog's idempotency.

---

### 2.3 PFCOUNT Execution Path

**Entry Point:** `pfcountCommand()` — `src/hyperloglog.c` line 1269

```c
void pfcountCommand(client *c) {

    // Case 1: Multiple keys → compute union cardinality
    if (c->argc > 2) {
        uint8_t max[HLL_HDR_SIZE + HLL_REGISTERS];
        memset(max, 0, sizeof(max));
        hdr->encoding = HLL_RAW;         // special internal encoding

        for (j = 1; j < c->argc; j++) {
            hllMerge(registers, o);      // MAX(register[i]) across all HLLs
        }
        addReplyLongLong(c, hllCount(hdr, NULL));
        return;
    }

    // Case 2: Single key → use cache or recompute
    o = lookupKeyRead(c->db, c->argv[1]);

    if (HLL_VALID_CACHE(hdr)) {
        // Cache valid → return instantly (O(1))
        card  = (uint64_t)hdr->card[0];
        card |= (uint64_t)hdr->card[1] << 8;
        // ... (reads 8-byte little-endian value from header)
        card |= (uint64_t)hdr->card[7] << 56;
    } else {
        // Cache invalid → recompute and cache result
        card = hllCount(hdr, &invalid);
        // save card back into hdr->card for next call
    }

    addReplyLongLong(c, card);
}
```

**Cache Lifecycle:**

```
PFADD myhll "apple"
      ↓ HLL_INVALIDATE_CACHE()    ← cache marked dirty

PFCOUNT myhll                     ← cache invalid
      ↓ hllCount()                ← expensive: reads 16,384 registers
      ↓ save result to hdr->card  ← cache now valid

PFCOUNT myhll (again)             ← cache valid
      ↓ return hdr->card instantly ← O(1), no recalculation ⚡

PFCOUNT myhll (1000 more times)   ← all return from cache instantly
```

---

### 2.4 The Estimation Formula — `hllCount()`

**Function:** `hllCount()` — `src/hyperloglog.c` line 1050

```c
uint64_t hllCount(struct hllhdr *hdr, int *invalid) {
    double m = HLL_REGISTERS;   // m = 16,384
    int reghisto[64] = {0};

    // Step 1: Build register histogram
    // reghisto[i] = number of registers with value i
    if (hdr->encoding == HLL_DENSE)
        hllDenseRegHisto(hdr->registers, reghisto);
    else if (hdr->encoding == HLL_SPARSE)
        hllSparseRegHisto(..., reghisto);
    else if (hdr->encoding == HLL_RAW)
        hllRawRegHisto(hdr->registers, reghisto);

    // Step 2: Apply improved HyperLogLog formula
    // Based on: Ertl (2017) arXiv:1702.01284
    double z = m * hllTau((m - reghisto[HLL_Q+1]) / (double)m);
    // hllTau() → correction for large cardinality (saturated registers)

    for (j = HLL_Q; j >= 1; --j) {
        z += reghisto[j];
        z *= 0.5;
    }
    // Core harmonic mean loop — weights each register by 2^(-value)

    z += m * hllSigma(reghisto[0] / (double)m);
    // hllSigma() → correction for small cardinality (many zero registers)

    // Step 3: Apply bias correction and return
    E = llroundl(HLL_ALPHA_INF * m * m / z);
    // HLL_ALPHA_INF = 0.7213475... (mathematically derived constant)
    // Final formula: E = α × m² / z
    return (uint64_t) E;
}
```

---

### 2.5 Complete End-to-End Flow Diagram

```
User types: PFADD myhll "apple"
                    │
                    ▼
         pfaddCommand()                    line 1229
         ├─ lookupKeyWrite()               find "myhll" in Redis DB
         ├─ createHLLObject()              if new → sparse HLL (18 bytes)
         └─ for each element:
                    │
                    ▼
              hllAdd()                     line 1088
              ├─ encoding == SPARSE? ──→  hllSparseAdd()
              └─ encoding == DENSE?  ──→  hllDenseAdd()
                                               │
                                               ▼
                                         hllPatLen()             line 452
                                         ├─ MurmurHash64A()      line 397
                                         │   └─ "apple" → 64-bit hash
                                         ├─ index = hash & HLL_P_MASK
                                         │   → first 14 bits → register #XXXX
                                         └─ count = leading zeros + 1
                                               │
                                               ▼
                                         hllDenseSet()
                                         ├─ count > oldcount? → update register
                                         │                     → return 1
                                         └─ count ≤ oldcount? → no change
                                                               → return 0
                    │
         HLL_INVALIDATE_CACHE()           mark cached count dirty
         addReply(1 or 0)                 send result to client

User types: PFCOUNT myhll
                    │
                    ▼
         pfcountCommand()                  line 1269
         ├─ argc > 2? ──→ hllMerge() all keys → hllCount()
         └─ single key:
                    │
                    ▼
              HLL_VALID_CACHE?
              ├─ YES → read hdr->card → return instantly ⚡
              └─ NO  → hllCount()                          line 1050
                            ├─ hllDenseRegHisto()          build histogram
                            ├─ hllTau()                    large cardinality fix
                            ├─ harmonic mean loop          core formula
                            ├─ hllSigma()                  small cardinality fix
                            └─ E = HLL_ALPHA_INF × m² / z → return estimate
                            │
                            └─ save result to hdr->card    update cache
```

---

## 3. Design Decisions

### Decision 1: MurmurHash64A as the Hash Function

**File:** `src/hyperloglog.c` line 397  
**Used at:** line 467 — `hash = MurmurHash64A(ele, elesize, 0xadc83b19ULL)`

**What problem it solves:**

HyperLogLog's mathematical accuracy depends entirely on uniform bit distribution across the 64-bit hash output. If certain elements cluster to the same register indices, the harmonic mean formula produces wildly inaccurate estimates. Every element must have an equal probability of mapping to any of the 16,384 registers.

**How it works:**

MurmurHash64A processes input 8 bytes at a time, applying a sequence of multiply-XOR-shift operations (the "avalanche effect") that ensures every input bit influences every output bit. The fixed seed `0xadc83b19ULL` ensures identical elements always hash identically across all Redis instances.

**Tradeoff introduced:**

MurmurHash64A is non-cryptographic — optimized for speed over security. It has a tiny theoretical collision probability. A cryptographic hash (SHA-256) would eliminate collisions entirely but run approximately 10x slower on every PFADD call. For cardinality estimation where 0.81% error is already inherent to the algorithm, the negligible collision risk is completely acceptable in exchange for the performance gain.

---

### Decision 2: Sparse → Dense Adaptive Encoding

**Files:** `src/hyperloglog.c` lines 586, 675 | `redis.conf` line 1992 | `src/config.c` line 3227

**The HLL Header Structure** (both encodings share this 16-byte header):

```
+------+---+-----+----------+
| HYLL | E | N/U | Cardin.  |
+------+---+-----+----------+
  4B    1B   3B      8B

HYLL    → magic bytes identifying this as an HLL object
E       → encoding: HLL_DENSE(0) or HLL_SPARSE(1)
N/U     → 3 unused/reserved bytes
Cardin. → 64-bit cached cardinality (MSB set = cache invalid)
```

**Sparse Encoding — Three Opcodes** (lines 365–386):

```
Bit pattern   Name    Size    Meaning
00xxxxxx  →  ZERO    1 byte  Next N registers are zero (N ≤ 64)
01xxxxxx  →  XZERO   2 bytes Next N registers are zero (N ≤ 16,384)
1vvvvvxx  →  VAL     1 byte  Next N registers all have value V (V ≤ 32, N ≤ 4)
```

Memory comparison:
```
Fresh HLL (0 elements):   [header 16B] + [XZERO:16384 = 2B] = 18 bytes total
After 3 elements:         [header 16B] + [XZERO][VAL][XZERO][VAL][XZERO] ≈ 26 bytes
Dense (always):           16B header + 16,384 × 6 bits = 12,304 bytes (≈ 12KB)
```

**Two Promotion Triggers** (one-way, irreversible):

```c
// Trigger 1: value too large for sparse (line 675)
if (count > HLL_SPARSE_VAL_MAX_VALUE) goto promote;
// VAL opcode only stores 5 bits → max value = 32
// Probability of this trigger: 1/2^32 (extremely rare)

// Trigger 2: size exceeds configured threshold
// server.hll_sparse_max_bytes = 3000 (default)
// Configured in redis.conf line 1992
// Runtime-changeable: CONFIG SET hll-sparse-max-bytes <value>
// Registered in config.c line 3227 with MODIFIABLE_CONFIG flag
```

**Promotion function:** `hllSparseToDense()` line 586 — allocates a fresh 12KB dense string, walks all sparse opcodes, sets non-zero registers in the dense array, then frees the old sparse data.

**What problem it solves:**

A system tracking unique visitors for 10 million web pages needs 10 million HLL counters. In sparse encoding, counters with few unique visitors use only 18–100 bytes each. Forcing 12KB dense for every HLL would consume 120GB for the same workload.

**Tradeoff introduced:**

Two completely separate code paths (sparse and dense) must be maintained throughout the codebase. Promotion is one-way — a dense HLL never returns to sparse even if usage drops. The sparse path also has higher per-operation CPU cost due to opcode decoding and potential buffer reallocation.

---

### Decision 3: Cardinality Result Cache in Header

**File:** `src/hyperloglog.c` — `pfaddCommand()` line 1229, `pfcountCommand()` line 1269

**Where implemented:**

```c
// The 8-byte Cardin. field in the header stores the cached result.
// MSB of the last byte set = cache invalid, clear = cache valid.

// Invalidated on every write — pfaddCommand() line ~1258:
HLL_INVALIDATE_CACHE(hdr);

// Checked on every read — pfcountCommand() line ~1317:
if (HLL_VALID_CACHE(hdr)) {
    card  = (uint64_t)hdr->card[0];
    card |= (uint64_t)hdr->card[1] << 8;
    card |= (uint64_t)hdr->card[2] << 16;
    card |= (uint64_t)hdr->card[3] << 24;
    card |= (uint64_t)hdr->card[4] << 32;
    card |= (uint64_t)hdr->card[5] << 40;
    card |= (uint64_t)hdr->card[6] << 48;
    card |= (uint64_t)hdr->card[7] << 56;
    // Return cached value instantly — no register scan needed
}
```

**What problem it solves:**

Computing `hllCount()` requires reading all 16,384 registers and applying the harmonic mean formula — a non-trivial operation at high call frequency. In production workloads such as a dashboard showing unique daily visitors, `PFCOUNT` may be called thousands of times per second while `PFADD` happens far less frequently. The cache converts repeated `PFCOUNT` calls from O(m) — where m=16,384 — to O(1).

**Tradeoff introduced:**

The cache is invalidated on every single `PFADD`, even if the element is a duplicate and no register actually changed. If `PFADD` and `PFCOUNT` strictly alternate at high frequency, the cache never provides benefit. The design is optimized specifically for read-heavy workloads and penalizes mixed write-read patterns.

---

## 4. Concept Mapping

### Concept 1: Storage — LSM Tree Analogy

| LSM Tree Concept | HyperLogLog Equivalent | Code Location |
|---|---|---|
| MemTable (small write buffer) | Sparse encoding (tiny, efficient) | lines 365–386 |
| SSTable (large, structured) | Dense encoding (fixed 12KB) | `HLL_DENSE_SIZE` |
| Compaction threshold | `hll-sparse-max-bytes = 3000` | redis.conf:1992 |
| One-way flush | `hllSparseToDense()` (irreversible) | line 586 |
| Bloom filter (approximate) | HLL estimate (probabilistic) | `hllCount()`:1050 |

Just as an LSM tree buffers writes in a small MemTable before flushing to a structured SSTable, HyperLogLog buffers small cardinalities in compact sparse encoding before promoting to the fixed-layout dense encoding. Both transitions are one-way and triggered by a configurable size threshold.

---

### Concept 2: Streaming / Ingestion

HyperLogLog is a textbook streaming algorithm satisfying all streaming constraints:

```
Streaming requirement          HyperLogLog behavior
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Single pass over data    →    hllAdd() processes each element once
Cannot store all data    →    element hashed → register updated → element discarded
O(1) memory              →    fixed 12KB regardless of stream length
Handles unbounded input  →    registers never overflow (max value = 63 in dense)
Supports merging         →    PFMERGE combines distributed stream counts
```

**Code evidence:** `hllAdd()` line 1088 — the element pointer is passed in, processed by `hllPatLen()`, and the element itself is never stored anywhere. Only the register value is updated. `MurmurHash64A()` at line 397 converts the element to a hash that is immediately used and discarded.

---

### Concept 3: Hash Partitioning

The 16,384 registers in HyperLogLog are functionally identical to hash partitions in a distributed system:

```
Hash Partitioning:                   HyperLogLog:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
partition_id = hash(key) % N    →    index = hash & HLL_P_MASK
N partitions                    →    16,384 registers
Each partition independent      →    each register tracks its own max
Balanced load by design         →    uniform hash ensures even distribution
```

**Code evidence** — `hllPatLen()` line 464:
```c
index = hash & HLL_P_MASK;
// HLL_P_MASK = 0x3FFF = binary 0000...0011111111111111
// Extracts first 14 bits → selects 1 of 2^14 = 16,384 registers
// HLL_P = 14 → log₂(16,384) = 14
```

The choice of 16,384 registers (2^14) is deliberate — it gives a standard error of `1.04 / √16384 = 0.81%`. More registers means lower error but higher memory cost.

---

### Concept 4: Fault Tolerance and Distributed Aggregation

PFMERGE enables a pattern of distributed fault-tolerant cardinality counting:

```
Architecture:
  Redis Node A (region East) → PFADD hll_east [user events]
  Redis Node B (region West) → PFADD hll_west [user events]
                   ↓
  PFMERGE total_users hll_east hll_west
                   ↓
  PFCOUNT total_users → accurate global unique user count
```

**Merge correctness** — `hllMerge()` line 1106:
```c
// For each of the 16,384 registers:
if (val > max[i]) max[i] = val;
// Taking MAX per register preserves the probabilistic property.
// MAX(register_A[i], register_B[i]) = the rarest element
// seen by EITHER node for that register partition.
// This is mathematically equivalent to having seen all elements
// on a single node — no accuracy is lost in the merge.
```

If one node fails and is rebuilt from scratch, its HLL simply contributes zeros to the merge — the surviving node's data is not corrupted. This makes the system resilient to partial node failure.

---

## 5. Experiments & Results

All experiments were run against Redis 7.2 compiled from source on Ubuntu 24, connecting via the Python `redis` client library.

## 5. Experiments & Results

### Experiment 1 — E-Commerce: Precision vs Memory Tradeoff (HLL_P Modification)

**Corporate Scenario:**
A large e-commerce platform needs to count unique daily visitors
per product page. With millions of product pages, choosing the
wrong `HLL_P` setting either wastes memory or produces inaccurate
analytics that directly affect revenue decisions.

**Code Modification — `src/hyperloglog.c`:**

```c
/* ORIGINAL */
#define HLL_P 14   /* 16,384 registers → ~0.81% error */

/* MODIFIED */
#define HLL_P 10   /* 1,024 registers  → ~3.25% error */
```

`HLL_P` controls register count as `2^HLL_P`.
Error formula: `1.04 / √(2^HLL_P)`
After each change Redis was recompiled (`make`) and restarted.

**Results — Before Modification (HLL_P = 14, Original):**

| N (Actual) | HLL Estimate | Error % |
|---|---|---|
| 100 | 100 | 0.00% |
| 1,000 | 995 | 0.50% |
| 5,000 | 5,009 | 0.18% |
| 10,000 | 9,961 | 0.39% |
| 50,000 | 50,138 | 0.28% |

**Results — After Modification (HLL_P = 10, Modified):**

| N (Actual) | HLL Estimate | Error % |
|---|---|---|
| 100 | 102 | 2.00% |
| 1,000 | 1,046 | 4.60% |
| 5,000 | 5,217 | 4.34% |
| 10,000 | 9,796 | 2.04% |
| 50,000 | 53,414 | **6.83%** |

**Before vs After Comparison:**

| N | Error (HLL_P=14) | Error (HLL_P=10) | Increase |
|---|---|---|---|
| 100 | 0.00% | 2.00% | +2.00% |
| 1,000 | 0.50% | 4.60% | +4.10% |
| 5,000 | 0.18% | 4.34% | +4.16% |
| 10,000 | 0.39% | 2.04% | +1.65% |
| 50,000 | 0.28% | **6.83%** | **+6.55%** |

**Analysis:**

Reducing `HLL_P` from 14 to 10 increased worst-case error from
**0.50% to 6.83%** — a 13.6x accuracy degradation. This directly
validates the theoretical formula:

- `HLL_P=14` → `1.04 / √16384 = 0.81%` (observed max: 0.50% ✅)
- `HLL_P=10` → `1.04 / √1024  = 3.25%` (observed max: 6.83%)

At `HLL_P=10`, error exceeded 3.25% in 3 of 5 runs due to
statistical variance — the theoretical value is a standard error
(1 sigma), not a hard ceiling. At `HLL_P=14`, all runs stayed
well within the 0.81% guarantee.

**Corporate Insight:**

| HLL_P | Registers | Error Guarantee | Memory/key | 10M pages total |
|---|---|---|---|---|
| 6 | 64 | ~13% | ~48 B | ~0.001 GB |
| 10 | 1,024 | ~3.25% | ~768 B | ~0.008 GB |
| **14** | **16,384** | **~0.81%** | **~12 KB** | **~0.134 GB** |

For billing and revenue analytics → `HLL_P=14` is required.
For product recommendations → `HLL_P=10` saves 94% memory
at acceptable 3.25% error.

**Code location:** `src/hyperloglog.c` — `#define HLL_P 14` (top of file)

---

### Experiment 2 — Social Media: Sparse-to-Dense Encoding Transition (HLL Threshold Modification)

**Corporate Scenario:**
A social media platform tracks unique hashtag interactions in
real-time. Redis HyperLogLog uses two internal encodings: a
memory-efficient **SPARSE** format for low-cardinality sets, and
a fixed **DENSE** (12 KB) format for high-cardinality sets.
Understanding when this transition occurs is critical for
capacity planning — an unexpected jump from ~800 B to 12 KB
per key at scale can cause memory spikes and OOM events.

**Code Modification — `src/hyperloglog.c`:**
```c

### Experiment 2 — Social Media: Sparse-to-Dense Encoding Transition (HLL Threshold Modification)

**Corporate Scenario:**
A social media platform tracks unique hashtag interactions in
real-time. Redis HyperLogLog uses two internal encodings: a
memory-efficient **SPARSE** format for low-cardinality sets, and
a fixed **DENSE** (12 KB) format for high-cardinality sets.
Understanding when this transition occurs is critical for
capacity planning — an unexpected jump from ~800 B to 12 KB
per key at scale can cause memory spikes and OOM events.

**Code Modification — `src/hyperloglog.c`:**
```c
/* ORIGINAL */
#define HLL_SPARSE_MAX_BYTES 3000   /* Sparse → Dense at ~3000 bytes */

/* MODIFIED */
#define HLL_SPARSE_MAX_BYTES 500    /* Hardcoded threshold = 500 bytes */
```

`HLL_SPARSE_MAX_BYTES` defines the maximum size (in bytes) the
sparse representation may occupy before Redis forcibly promotes
the structure to the dense 12 KB layout.
After each change Redis was recompiled (`make`) and restarted.

**Results — Before Modification (Threshold = 3000 bytes, Original):**

| N (Actual) | Memory (B) | HLL Count | Encoding |
|---|---|---|---|
| 10  | 152   | 10  | SPARSE |
| 50  | 248   | 50  | SPARSE |
| 100 | 440   | 100 | SPARSE |
| 200 | 824   | 200 | SPARSE |
| 500 | 1,336 | 497 | SPARSE |
| 1,000 | 2,616 | 999 | SPARSE |

All entries remain in SPARSE encoding under the original 3000-byte
threshold — none of the tested cardinalities triggered promotion.

**Results — After Modification (Threshold = 500 bytes, Modified):**

| N (Actual) | Memory (B) | HLL Count | Encoding |
|---|---|---|---|
| 10  | 152    | 10   | SPARSE      |
| 50  | 248    | 50   | SPARSE      |
| 100 | 440    | 100  | SPARSE      |
| 200 | 824    | 198  | SPARSE      |
| 500 | 14,392 | 497  | DENSE(12KB) |
| 1,000 | 14,392 | 1,001 | DENSE(12KB) |

With the threshold lowered to 500 bytes, the sparse-to-dense
transition is triggered between N=200 and N=500, causing memory
to jump from **824 B → 14,392 B** — a **17.5× increase**.

**Before vs After Comparison:**

| N | Memory Before (B) | Memory After (B) | Encoding Before | Encoding After |
|---|---|---|---|---|
| 10  | 152   | 152    | SPARSE | SPARSE      |
| 50  | 248   | 248    | SPARSE | SPARSE      |
| 100 | 440   | 440    | SPARSE | SPARSE      |
| 200 | 824   | 824    | SPARSE | SPARSE      |
| 500 | 1,336 | 14,392 | SPARSE | DENSE(12KB) |
| 1,000 | 2,616 | 14,392 | SPARSE | DENSE(12KB) |

**Analysis:**
Lowering `HLL_SPARSE_MAX_BYTES` from 3000 to 500 caused premature
dense promotion. At N=500, memory consumption increased **17.5×**
(1,336 B → 14,392 B). Once in DENSE mode, memory stays fixed at
~14 KB regardless of whether N=500 or N=1,000 — confirming the
dense layout pre-allocates the full 12 KB register array
regardless of actual cardinality.

Under the original threshold, the same workload (N up to 1,000)
consumed at most **2,616 B** — over **5× less memory** than the
modified configuration's 14,392 B.

**Corporate Insight:**

| Threshold | Transition Point | Memory at N=1000 | Risk |
|---|---|---|---|
| 500 B  | N ≈ 200–500   | 14,392 B | Premature promotion, memory spikes |
| 3000 B | N > 1,000     | 2,616 B  | Controlled, gradual growth |
| 3000 B (default) | Optimal for most workloads | Efficient | ✅ Recommended |

For social media hashtag counters with millions of keys:
- A **premature threshold** (500 B) forces nearly all real-world
  keys into DENSE format, multiplying memory usage 5–17×.
- The **default threshold** (3000 B) keeps low-to-moderate
  cardinality hashtags in SPARSE format, deferring the 12 KB
  allocation until genuinely needed.
- At 10M hashtag keys: default saves ~116 GB vs. the 500-byte config.

**Code location:** `src/hyperloglog.c` — `#define HLL_SPARSE_MAX_BYTES 3000`
---
### Experiment 3 — FinTech: Real-Time Credit Card Fraud Detection (HLL + Keyspace Notifications)

**Corporate Scenario:**
A financial institution processes millions of credit card
transactions daily. A card accessed from too many unique
IP locations within a session is a strong fraud signal.
Traditional exact-count approaches require storing every
IP per card — costly at scale. This experiment demonstrates
how Redis HyperLogLog enables **probabilistic fraud detection**
with O(1) memory per card, combined with **keyspace notifications**
to trigger real-time server-side alerts the moment a fraud
threshold is crossed.

A custom threshold check was injected so that every `PFADD`
triggers a server-side log message when `PFCOUNT` exceeds 10
unique locations — simulating a fraud alert pipeline without
any client-side polling.

**Simulation Setup:**

| Card ID | Unique IPs Injected | Expected Status |
|---|---|---|
| `card:4111-1111-NORMAL` | 4  | ✅ SAFE        |
| `card:5500-0000-SUSPECT` | 8  | ⚠️ SUSPICIOUS  |
| `card:3714-4963-FRAUD`   | 15 | 🚨 FRAUD ALERT |

Threshold logic applied:
- `unique_locs > 10` → **FRAUD ALERT**
- `unique_locs > 7`  → **SUSPICIOUS**
- Otherwise          → **SAFE**

**Program (Client-side):**
```python
import redis, time
r = redis.Redis(decode_responses=True)

cards = {
    "card:4111-1111-NORMAL":  4,   # 4 unique IPs  → safe
    "card:5500-0000-SUSPECT": 8,   # 8 unique IPs  → borderline
    "card:3714-4963-FRAUD":   15   # 15 unique IPs → FRAUD!
}

for card_key, num_locations in cards.items():
    r.delete(card_key)
    locations = [f"192.168.{i}.{i*3}" for i in range(num_locations)]

    for ip in locations:
        r.pfadd(card_key, ip)

    unique_locs = r.pfcount(card_key)
    status = ("🚨 FRAUD ALERT" if unique_locs > 10
              else "⚠️ SUSPICIOUS" if unique_locs > 7
              else "✅ SAFE")

    print(f"\nCard:      {card_key}")
    print(f"Locations: {unique_locs} unique IPs")
    print(f"Status:    {status}")
    time.sleep(1)
```

**Results — Client Terminal Output:**

| Card | Unique IPs Detected | Status |
|---|---|---|
| `card:4111-1111-NORMAL`  | 4  | ✅ SAFE        |
| `card:5500-0000-SUSPECT` | 8  | ⚠️ SUSPICIOUS  |
| `card:3714-4963-FRAUD`   | 15 | 🚨 FRAUD ALERT |

**Results — Redis Server Terminal (Keyspace Notification Logs):**

[FRAUD ALERT] Card 'card:3714-4963-FRAUD' accessed from 11 unique locations!
[FRAUD ALERT] Card 'card:3714-4963-FRAUD' accessed from 12 unique locations!
[FRAUD ALERT] Card 'card:3714-4963-FRAUD' accessed from 13 unique locations!
[FRAUD ALERT] Card 'card:3714-4963-FRAUD' accessed from 14 unique locations!
[FRAUD ALERT] Card 'card:3714-4963-FRAUD' accessed from 15 unique locations!

Server-side alerts fired **5 consecutive times** as the fraud
card crossed and continued past the 10-location threshold —
demonstrating real-time, incremental alerting with no client
polling required.

**Analysis:**
- HyperLogLog consumed **constant ~14 KB** per card regardless
  of whether 4 or 15 IPs were tracked — no per-IP storage needed.
- The normal card (4 IPs) and suspect card (8 IPs) produced
  **zero server-side alerts**, confirming threshold precision.
- The fraud card triggered alerts incrementally from location
  11 through 15, enabling downstream systems (e.g., card freeze,
  SMS alert) to react **as fraud unfolds**, not after the fact.
- `PFADD` + `PFCOUNT` operations are both **O(1)** — the fraud
  check adds negligible latency to the transaction pipeline.

**Corporate Insight:**

| Approach | Memory per Card | Fraud Latency | Scalability |
|---|---|---|---|
| Exact Set (Redis SET) | O(n) per IP stored | Post-hoc query | Poor at 100M cards |
| HLL + Notifications   | ~14 KB fixed       | Real-time, in-flight | ✅ Constant |

For a bank processing 100M active cards:
- Exact tracking: potentially **gigabytes** of IP sets per day.
- HLL approach: **~1.4 TB fixed** regardless of transaction volume,
  with fraud alerts delivered at the moment of threshold breach.

---

## 6. Failure Analysis

---

## 8. GitHub Artifacts

```
BDE_Project/
├── README.md                          ← this report
├── redis/                             ← Redis 7.2 source (cloned)
│   └── src/hyperloglog.c              ← primary file studied
└── experiments/
    ├── experiment1_error_rate.py      ← error rate vs cardinality
    ├── experiment2_memory.py          ← HLL vs SET memory
    ├── experiment3_skew.py            ← duplicate/skew handling
    └── experiment4_sparse_threshold.py ← threshold modification
```

### Quick Reproduction

```bash
# Clone and build Redis
git clone https://github.com/redis/redis.git
cd redis && git checkout 7.2 && make

# Start server
src/redis-server &

# Install Python client
pip3 install redis

# Run all experiments
cd experiments
python3 experiment1_error_rate.py
python3 experiment2_memory.py
python3 experiment3_skew.py
python3 experiment4_sparse_threshold.py
```

---

## References

1. Redis 7.2 Source Code — `src/hyperloglog.c` — https://github.com/redis/redis

2. Redis Configuration Reference — `redis.conf` line 1992 — `hll-sparse-max-bytes`
