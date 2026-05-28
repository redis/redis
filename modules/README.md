# Redis Modules

This directory bundles external Redis modules as source-pinned dependencies.
Each module is cloned from its upstream repo into `modules/<name>/src/`,
built against the Redis in this tree, and loaded into `redis-server` at
runtime.

For the deep dive on layout, manifest format, and every flag, see
[MODULES.md](MODULES.md). This README is the short version: the working
flow plus pointers upstream.

## Bundled modules

| Module | Purpose | Upstream repo | Pinned `ref` |
|---|---|---|---|
| [redisbloom](redisbloom/) | Probabilistic data structures (Bloom, Cuckoo, Count-Min, Top-K, t-digest) | https://github.com/redisbloom/redisbloom | `master` |
| [redisearch](redisearch/) | Full-text search, secondary indexing, vector search | https://github.com/redisearch/redisearch | `v8.7.90` |
| [redisjson](redisjson/) | Native JSON data type and JSONPath queries | https://github.com/redisjson/redisjson | `master` |
| [redistimeseries](redistimeseries/) | Time-series data type with downsampling and aggregation | https://github.com/redistimeseries/redistimeseries | `master` |
| [vector-sets](vector-sets/) | In-tree vector set data type (not cloned) | *(lives in this repo)* | — |

The authoritative pin list is [modules.yaml](modules.yaml). To bump a
module, edit its `ref:` there and run `make modules-update <name>`.

> **macOS:** use `gmake` instead of `make` (GNU Make ≥ 4.x). All examples
> below say `make`; substitute `gmake` on macOS.

## The flow

```
       ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
       │   setup      │ →  │   build      │ →  │   run        │
       │ clone + deps │    │ redis + .so  │    │ load + start │
       └──────────────┘    └──────────────┘    └──────────────┘
                                   │
                                   ↓
                            ┌──────────────┐
                            │    test      │
                            │  per-module  │
                            └──────────────┘
```

### 1. First-time provisioning — `make modules-update` + `make bootstrap`

`modules-update` clones every module listed in [modules.yaml](modules.yaml)
into `modules/<name>/src/` at its pinned ref. `bootstrap` then runs each
module's `.install/install_script.sh` to install OS packages, set up a
Python venv, and pull in any module-specific toolchain (e.g. Rust for
`redisjson`).

```bash
make modules-update    # clone all modules from modules.yaml
make bootstrap         # install per-module deps for every cloned module
```

Pass module names to either step to scope it: `make modules-update redisbloom redisjson` /
`make bootstrap redisbloom redisjson`. Use `make bootstrap` on its own to re-run just
the dependency install.

### 2. Build — `make build`

Builds Redis first, then each cloned module against that Redis. After
the build, regenerates `redis-gen.conf` so the runtime config reflects
what was actually built.

```bash
make build                         # Redis + every module
make build redis                   # Redis only
make build redistimeseries         # Redis + one module
```

### 3. Run — `make run`

Starts `src/redis-server` with the selected modules auto-loaded via
`--loadmodule`. The `.so` path is discovered by `find` under
`modules/<name>/`, so it works across macOS and Linux without
hardcoding platform paths.

```bash
make run                                   # all built modules
make run redistimeseries redisbloom        # subset
make run ARGS="--port 6400 --loglevel debug"
```

Verify from another shell:

```bash
redis-cli MODULE LIST
```

### 4. Update pinned versions — `make modules-update`

Idempotent: clones if the module isn't on disk, otherwise moves the
existing checkout to the current pin. Run after editing `ref:` in
[modules.yaml](modules.yaml).

```bash
make modules-update redisbloom     # bump one
make modules-update                # refresh every module
```

### 5. Test — `make test`

```bash
make test                          # Redis core tests
make test redistimeseries          # one module's full suite
make test redistimeseries ts_info  # one named test
make test all                      # every module (continues past failures)
```

Test names containing `:` must be passed as `TEST=…` rather than
positionally — Make reserves `:` for rule syntax. See
[MODULES.md §7](MODULES.md#7-test-make-test) for the full rules.

## Typical iteration loop

```bash
# One-time:
make modules-update
make bootstrap
make build

# Day to day:
make modules-update redisbloom        # after editing ref: in modules.yaml
make build                            # rebuild + refresh redis-gen.conf
make run redistimeseries redisbloom   # start with just these two
redis-cli MODULE LIST
make test redistimeseries             # exercise the module
```

## Configuration: `redis.conf` vs. `redis-gen.conf`

- `redis.conf` — tracked, hand-edited Redis-core config. **Do not** add
  module load lines here.
- `redis-gen.conf` — untracked, regenerated on every `make build` /
  `make modules-update`. This is the file that actually loads the
  bundled modules. It's `redis.conf` plus a `loadmodule` line and
  inlined `module.conf` for each built module.

Point `redis-server` at it explicitly when you want the generated
config:

```bash
./src/redis-server redis-gen.conf
```

Or promote it onto `redis.conf` once for a one-file launch path (see
[MODULES.md §6.1](MODULES.md#61-promoting-the-generated-config-make-promote-redis-conf--overwrites-redisconf)):

```bash
make promote-redis-conf
./src/redis-server redis.conf
```

## Further reading

- [MODULES.md](MODULES.md) — full reference: manifest format, ref
  resolution, shallow clones, generated-config internals, release
  tarballs (`make tarball`), test-name quirks.
- [modules.yaml](modules.yaml) — the pin manifest itself.
- Redis Modules API docs:
  https://redis.io/docs/latest/develop/reference/modules/
