# Redis Modules

This directory bundles external Redis modules as source-pinned dependencies.
Each module is cloned from its upstream repo into `modules/<name>/src/`,
built against the Redis in this tree, and loaded into `redis-server` at
runtime.

For the deep dive on layout, manifest format, and every flag, see
[MODULES.md](MODULES.md). This README is the short version: the working
flow plus pointers upstream.

## Bundled modules

| Module | Purpose | Upstream repo |
|---|---|---|
| [redisbloom](redisbloom/) | Probabilistic data structures (Bloom, Cuckoo, Count-Min, Top-K, t-digest) | https://github.com/redisbloom/redisbloom |
| [redisearch](redisearch/) | Full-text search, secondary indexing, vector search | https://github.com/redisearch/redisearch |
| [redisjson](redisjson/) | Native JSON data type and JSONPath queries | https://github.com/redisjson/redisjson |
| [redistimeseries](redistimeseries/) | Time-series data type with downsampling and aggregation | https://github.com/redistimeseries/redistimeseries |
| [vector-sets](vector-sets/) | In-tree vector set data type (not cloned) | *(lives in this repo)* |

The authoritative pin list is [modules.yaml](modules.yaml). To bump a
module, edit its `ref:` there and run `make modules-update <name>`.

## Building from a release tarball

A release tarball (from `make tarball`) already bundles every module's
source under `modules/<name>/src/`, so **do not** run `make modules-update`
there — the sources are present and pinned. Just build:

```sh
cd redis-<version>
export BUILD_TLS=yes INSTALL_RUST_TOOLCHAIN=yes
make -j "$(nproc)" all
```

`make modules-update` is only for the from-source dev flow below, where
modules are cloned fresh from their upstream repos.


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

> **Note:** `make bootstrap` provisions the dependencies for a **non-LTO
> build** on all supported OSes. The default build is now **`LTO=1`**, which
> needs an extra, tightly-matched toolchain (Clang + lld at the LLVM version
> Rust was built with) that `make bootstrap` does **not** install — if you
> want the LTO build, install those LTO dependencies manually first.
> For the bootstrap-supported non-LTO build, pass `LTO=0` explicitly:
> `make build LTO=0`.

### 2. Build — `make build`

Builds Redis first, then each cloned module against that Redis.

```bash
make build                         # Redis + all modules
make build core                    # Redis only
make build redistimeseries         # Redis + one module
```

### 3. Run — `make run`

Starts `src/redis-server` with the selected modules auto-loaded via
`--loadmodule`. The `.so` path is discovered by `find` under
`modules/<name>/`, so it works across macOS and Linux without
hardcoding platform paths.

```bash
./src/redis-server redis-full.conf          # all modules + redis configs
./src/redis-server redis.conf               # redis core only, no modules
make run                                     # all built modules without configs
make run redistimeseries redisbloom          # subset
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
make build                            # rebuild
make run redistimeseries redisbloom   # start with just these two
redis-cli MODULE LIST
make test redistimeseries             # exercise the module
```

## Configuration: `redis.conf` vs. `redis-full.conf`

- `redis.conf` — tracked, hand-edited Redis-core config. **Do not** add
  module load lines here.
- `redis-full.conf` — untracked, regenerated on every `make modules-update`. This is the file that actually loads the
  bundled modules. It's `redis.conf` plus a `loadmodule` line and
  inlined `module.conf` for each built module.

Point `redis-server` at it explicitly when you want the generated
config:

```bash
./src/redis-server redis-full.conf
```

Release tarballs (built with `make tarball`) bake the module config straight
into `redis.conf` during packaging (see
[MODULES.md §6.1](MODULES.md#61-baking-the-module-config-into-redisconf)), so
from an extracted tarball you can also run:

```bash
./src/redis-server redis.conf
```

## IDE setup (VSCode / Cursor)

Module sources live at `modules/<name>/src/`, which is 3 levels deep. By default VSCode/Cursor only scans 1 level for Git repos and will miss them.

**Option A — single-folder setup** (all modules share one Git sidebar):

Add to your local `.vscode/settings.json` (do not commit this file):
```json
{
    "git.repositoryScanMaxDepth": 3
}
```

**Option B — workspace setup** (each module appears as its own project with its own Git context):

Create `redis.code-workspace` at the repo root with the following content, then open it in VSCode/Cursor:

```json
{
    "folders": [
        { "name": "redistimeseries", "path": "modules/redistimeseries/src" },
        { "name": "redisbloom",      "path": "modules/redisbloom/src" },
        { "name": "redisjson",       "path": "modules/redisjson/src" },
        { "name": "redisearch",      "path": "modules/redisearch/src" },
        { "name": "modules",         "path": "modules" },
        { "name": "redis (root)",    "path": "." }
    ],
    "settings": {
        "git.autoRepositoryDetection": true,
        "git.repositoryScanMaxDepth": 1,
        "git.openRepositoryInParentFolders": "never",
        "git.detectSubmodules": true,
        "git.repositoryScanIgnoredFolders": ["node_modules", "deps", "bin", "build", ".build"]
    }
}
```

---

## Further reading

- [MODULES.md](MODULES.md) — full reference: manifest format, ref
  resolution, shallow clones, generated-config internals, release
  tarballs (`make tarball`), test-name quirks.
- [modules.yaml](modules.yaml) — the pin manifest itself.
- Redis Modules API docs:
  https://redis.io/docs/latest/develop/reference/modules/
