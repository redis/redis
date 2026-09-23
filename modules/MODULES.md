# External Modules: build, run, and test

This document describes the Makefile additions that manage external Redis
modules as source-pinned dependencies — cloned into each module's own
`modules/<name>/src/` directory, built against this repo's Redis, loaded
into `redis-server` at runtime, and tested from one top-level entry
point.

All commands are invoked at the repo root. On macOS, you must use
`gmake` (GNU Make ≥ 4.x) instead of `make`, because upstream module
build systems rely on features missing from macOS's default Make 3.81.
Everywhere below, `make` means "GNU make"; substitute `gmake` on macOS.

---

## 1. Layout

| Path | Role |
|---|---|
| `modules/modules.yaml` | **Single source of truth** for which upstream repo and `ref:` (tag, branch, or commit SHA) each external module is pinned to, plus the per-module build artifact path (`target_module`) and runtime load path (`loadmodule`). |
| `modules/manifest.mk` | Thin Make-API wrapper. Exposes `AVAILABLE_MODULES`, `$(call manifest-field,…)`, `$(call manifest-ref,…)`, `$(call manifest-ref-kind,…)`. All actual YAML parsing is delegated to `scripts/lib/manifest.sh`. |
| `modules/common.mk` | Shared build rules for every module — invoked via `make -C modules/<name> -f common.mk` from `modules/Makefile` or `scripts/build.sh`. Auto-derives `MODULE_NAME` from `$(notdir $(CURDIR))`; reads repo / ref / `target_module` from the manifest via `manifest.mk`'s helpers. (`MODULE_NAME` is intentionally **not** passed on the cmdline — doing so would propagate via `MAKEFLAGS` and clobber per-module Makefiles that use `MODULE_NAME` as their build-output basename, e.g. redisjson's `rejson.so`.) |
| `modules/Makefile` | Per-module dispatcher. Iterates `$(AVAILABLE_MODULES)`; for each, runs `make -C <name> -f common.mk`. Also owns optional Rust-toolchain install and `-Werror` stripping. |
| `modules/<name>/src/` | Checkout location — created by `make modules-update`. Bundled modules carry no per-module Makefile; `common.mk` plus the manifest is enough. |
| `scripts/lib/manifest.sh` | **Single source of truth for YAML parsing.** Dual-mode: sourced as a shell library by every other script (`manifest_modules`, `manifest_field`, `manifest_ref`, `resolve_modules`, …) and invoked as a CLI by `manifest.mk`. |
| `scripts/build.sh` etc. | One script per top-level target (`build`, `deps`, `run`, `test`, `modules-update`, `modules-shallow`, `sync-redis-conf`, `tarball`). The top-level `Makefile` exposes each as a 1–3-line target that shells out here. |
| `scripts/sync-redis-conf.sh` | Generates `redis-full.conf` from `redis.conf` + per-module `module.conf` files. See §6. |
| `redis.conf` | Tracked, hand-edited Redis-core config (stock upstream layout — no markers). `sync-redis-conf` emits it verbatim and appends the Modules section after it; don't hand-add `loadmodule` lines. |
| `redis-full.conf` | Untracked, regenerated on every `make modules-update`. This is the file you actually load. |
| `src/redis-server` | Redis binary — produced by `make`. |

Only modules listed in `modules.yaml` are considered "external" — those
are the ones managed by `make modules-update`. In-tree modules (e.g.
`vector-sets`) are intentionally absent from the manifest and live under
`modules/<name>/` without a `src/` subdirectory or a manifest entry.

### Pin manifest (`modules.yaml`)

Single file, one entry per managed module. Each entry pins the module to
exactly one `ref:` (a tag, branch, or commit SHA), and names the build
artifact it produces:

```yaml
modules:
  - name: redisearch
    repo: https://github.com/redisearch/redisearch
    ref: v8.7.90                                   # tag, branch, or commit SHA
    target_module: search-community/redisearch.so  # produced under src/bin/<variant>/
    loadmodule: ./modules/redisearch/redisearch.so # post-build copy used by redis-full.conf
```

**Ref kind** is resolved dynamically against `repo:` by querying
`git ls-remote`, in this order — first match wins:

1. **tag** — `refs/tags/<ref>` exists on the remote (preferred for releases).
2. **branch** — `refs/heads/<ref>` exists on the remote.
3. **commit** — the value looks like a hex SHA (7–40 chars) when neither
   of the above matched.

`ref:` must be non-empty; otherwise `make modules-update` errors loudly.
If no kind matches, the error message names the ref and the repo so you
can correct the manifest.

**`target_module`** is the path to the `.so` the upstream build produces,
**relative to `src/bin/<os>-<arch>-release/`**. Most modules are just
`<name>.so`; `redisjson` produces `rejson.so`; `redisearch` nests under
`search-community/`. Required — `make` errors loudly if it's missing.

**`loadmodule`** is the runtime path emitted into `redis-full.conf`. By
convention it's `./modules/<name>/<artifact-basename>` (where the build
step `cp`s the just-built `.so`).

To bump a module: edit `ref:` and run
`make modules-update <name>`. That's the only place pins live.

### Why `manifest.mk` and `manifest.sh` both exist

Make cannot source shell libraries at parse time — it can only consume
strings from `$(shell …)` invocations. So `scripts/lib/manifest.sh`
holds the parser (single source of truth), and `modules/manifest.mk`
is a 20-line Make-API binding that calls into it via `$(shell …)`.
Same shape as the rest of the build system: thin Make target → script
with the real logic.

---

## 2. Clone / update: `make modules-update`

```bash
make modules-update [<name> ...]
```

A single idempotent command: clones the module into `modules/<name>/src/`
at the pinned ref if it isn't there yet, otherwise moves the existing
clone to the current pin and re-syncs its submodules. Safe to re-run.

Name expansion:

| Argument | Selects |
|---|---|
| _(no args)_ | Every module listed in `modules.yaml` (default) |
| `<name>` | One module (e.g. `redistimeseries`) |
| `all` / `.` | Same as no args |

Examples:

```bash
make modules-update                                    # every module
make modules-update redistimeseries
make modules-update redisbloom redisearch redisjson
make modules-update all                                # explicit synonym for no-args
```

The clone location is fixed at `modules/<name>/src/`. After every
successful run, `modules/<name>/src/.prepared` is touched so
`common.mk`'s prepare step is satisfied and won't try to re-clone on
subsequent builds.

After cloning, `make modules-update` invokes `make sync-redis-conf` to
regenerate `redis-full.conf` from the updated state (see §6).

### Shallow clones: `make modules-shallow`

`make modules-update` clones with full history by default so tools like
Git Graph, GitLens, or `git log` work as expected. To clone shallow
(`--depth 1`) instead — useful on CI, or when disk/bandwidth matter — pass
the flag at install time:

```bash
make modules-update redistimeseries MODULES_UPDATE_SHALLOW=1
```

`make tarball` runs `modules-update all MODULES_UPDATE_SHALLOW=1`
internally before staging — full history would just inflate the staging
clone without ending up in the tarball.

To shrink an already-installed module back to a shallow clone:

```bash
make modules-shallow redistimeseries
make modules-shallow redisbloom redisearch
make modules-shallow all
```

Implementation note: `modules-shallow` removes the existing
`modules/<name>/src` and re-runs `modules-update` with
`MODULES_UPDATE_SHALLOW=1`, since re-shrinking an existing full clone in
place is unreliable. Any uncommitted changes inside `modules/<name>/src`
will be lost.

---

## 3. Install per-module deps: `make bootstrap`

Each cloned module ships its own `.install/install_script.sh` that
auto-detects the host OS (Ubuntu, Alpine, AzureLinux, Mariner, macOS,
…) and installs build/test prerequisites for that OS. `make bootstrap`
dispatches to every selected module's installer in turn:

```bash
make bootstrap [<name> ...|all|.]
```

| Argument | Selects |
|---|---|
| _(no args)_ / `all` / `.` | Every cloned module |
| `<name> [<name> ...]` | Only the listed modules |

Each module's installer is self-contained — package manager, Python venv
(under `modules/<name>/src/venv/`), test deps, plus any module-specific
toolchain (e.g. Rust for `redisjson`). The dispatcher continues past
failures and prints a summary, so one broken module doesn't block the
rest.

For a fresh checkout, run `make modules-update` to clone every module
listed in `modules.yaml`, then `make bootstrap` to install per-module
deps. Re-run `make bootstrap` whenever deps need a refresh (e.g. after
a Python version change).

---

## 4. Build: `make build [<name>]`

```bash
make build                        # Redis + all cloned modules
make build core                   # Redis only
make build redistimeseries        # Redis + one module
make build -j8                    # parallel build
```

All forms route through `scripts/build.sh`.

Selection:

| Command | Selects |
|---|---|
| `make build` / `make build all` / `make build .` | Redis + every cloned module |
| `make build core` / `make build redis` / `make build none` | Redis only; skip modules |
| `make build <name>` | Redis + only the named module |

Invalid module names are detected **before** any compilation runs — you
won't waste time on a Redis rebuild just to hit a typo at the end.

Order (deliberate):

1. Validate selection.
2. Build Redis (`$(MAKE) -C src all`). If this fails, nothing else runs.
3. For each selected cloned module, invoke `$(MAKE) -C modules/<name>`
   using `modules/common.mk` with:
   ```make
   REDIS_SERVER=<repo>/src/redis-server
   ```
   So module test harnesses use the Redis we just built. The include-path
   vars (`RM_INCLUDE_DIR`, `RS_INCLUDE_DIR`) are **not** overridden — each
   module resolves `redismodule.h` via its own upstream defaults, the
   same way it would build standalone in its own repo. Override them on
   the command line — `make build redistimeseries VAR=…` — if you need to
   compile a module against this tree's `redismodule.h`.
4. Build stops on the first failing module (fail-fast).
5. Final output lists `src/redis-server` plus every `.so` produced per
   module.

Examples:

```bash
make build                              # Redis + all cloned modules
make build core                         # Redis only
make build redistimeseries              # Redis + just one module
```

---

## 5. Run: `make run`

```bash
make run [<name> ...] [ARGS="<redis-server args>"]
```

Starts `src/redis-server` and auto-loads modules via `--loadmodule`.

Selection:

| Argument | Selects |
|---|---|
| *(none)* | Load every cloned module |
| `all` / `.` | Same as *(none)* |
| `none` | Start Redis with no modules |
| `<name> [<name> ...]` | Load only the listed modules |

The `.so` path is discovered via `find` under `modules/<name>/` using
the filename from `target_module` (e.g. `rejson.so` for redisjson), so
**no hardcoded platform paths** — it works across macOS and Linux
regardless of each module's `FULL_VARIANT` naming
(`macos-arm64v8-release`, `linux-x64-release`, etc.). Release builds
are preferred over debug builds; `CMakeFiles/`, `tests/`, `samples/`
are excluded.

`make run` builds `--loadmodule` flags inline and execs
`src/redis-server` directly — it does **not** consume `redis-full.conf`.
If you want the generated config (with each module's `module.conf`
inlined, see §6), point `redis-server` at it explicitly:

```bash
./src/redis-server redis-full.conf
# or:
make run none ARGS="redis-full.conf"
```

Extra `redis-server` flags/config go through `ARGS`.

Examples:

```bash
make run                                              # all built modules, default port
make run redistimeseries                              # single module
make run redistimeseries redisbloom                   # subset
make run none                                         # bare redis-server
make run ARGS="--port 6400 --loglevel debug"          # all modules + custom args
make run redistimeseries ARGS="--port 6400"           # one module + custom args
make run none ARGS="redis-full.conf --appendonly yes"  # use the generated config
```

Verification from another shell:

```bash
redis-cli -p 6379 MODULE LIST
```

If a module is cloned but not built, `make run` prints a warning and
skips it — it does not stop the other loads.

---

## 6. Generated config: `make sync-redis-conf` → `redis-full.conf`

```bash
make sync-redis-conf [<name> ...] [MODULES="<names>"] [ASSUME_BUILT=1|true|yes]
```

Rewrites the untracked `redis-full.conf` file at the repo root from:

1. **`redis.conf`** — emitted verbatim as the core config. Any Modules
   block appended by a previous run (fenced by
   `# >>> BEGIN section: Modules … <<<` / matching `END`) is stripped
   first, so regeneration is idempotent and never duplicates it.
2. **`modules.yaml`** — for each selected module:
   - If the `.so` artifact (manifest's `loadmodule` field) is on disk
     (or `ASSUME_BUILT=1`), emit an active `loadmodule <so>` line and
     inline `modules/<name>/src/module.conf`.
   - Otherwise, emit a commented placeholder and do **not** inline
     `module.conf` (those directives would break `redis-server` when
     the module isn't loaded).
   - Modules with no `loadmodule:` field show up as `Bad manifest: <name>`
     in the file header so misconfigurations surface loudly.
   - **Path rewriting**: in dev mode (no `PREFIX`) the manifest's
     `loadmodule:` value is written as-is — relative paths stay
     relative, so `redis-server redis-full.conf` works when launched
     from the source/tarball root. When `PREFIX` is set (install
     mode), paths become `$PREFIX/<basename>` — used by `make deploy`
     to point at the installed modules directory.
     File existence is tested against the on-disk path (REPO_ROOT-relative).

The write is atomic (tmpfile + `mv`), so a mid-run failure leaves the
previous `redis-full.conf` intact.

| Variable | Default | Purpose |
|---|---|---|
| `MODULES` | every module in `modules.yaml` | Subset of modules to include. Unrequested modules are omitted entirely (not even commented out). |
| `ASSUME_BUILT` | unset | When `1` / `true` / `yes`, emit active `loadmodule` lines regardless of whether the `.so` is on disk. Used by `make tarball`. |
| `PREFIX` | `$PWD` (= repo root) | Install root prepended to every emitted `loadmodule` path. `./modules/foo/foo.so` → `$PREFIX/modules/foo/foo.so`. Pass on the make line: `make PREFIX=/opt/redis-deploy`. |
| `REDIS_CONF` | `redis.conf` | Source Redis-core config |
| `REDIS_GEN_CONF` | `redis-full.conf` | Destination |

`make sync-redis-conf` runs automatically at the end of
`make modules-update`, so most users never invoke it directly. Run it by hand only if you've edited `modules.yaml` /
`module.conf` and want an immediate refresh, or if you want to scope
the file to a specific module subset.

## 6.1 Baking the module config into `redis.conf`

`make tarball` produces a release tarball whose `redis.conf` already contains
the full module configuration — the Redis-core config with the generated Modules
block (loadmodule lines + per-module settings) appended after it. So a user who
extracts the tarball can run `./src/redis-server redis.conf` directly and get the
bundled modules, without pointing at `redis-full.conf`.

This baking happens **only** during `make tarball` (inside its staging tree).
There is no `make` target that rewrites your local `redis.conf`, and the
`redis.conf` tracked in the repo is never modified by the normal flow. For local
runs, use the auto-generated `redis-full.conf`
(`./src/redis-server redis-full.conf`).

It's idempotent: the Modules block is fenced by
`# >>> BEGIN section: Modules … <<<` / matching `END`, and each regeneration
strips any previous block before re-appending a fresh one, so it never
duplicates.

### Per-module private blocks

Anything in a module's `module.conf` wrapped in:

```conf
# >>> BEGIN redis-gen-conf:private <<<
…
# <<< END redis-gen-conf:private <<<
```

(including the marker lines themselves) is stripped on inline. Use
this for internal/advanced tunables that you don't want exposed in the
bundled `redis-full.conf`. Multiple blocks per file are fine; nesting
is not supported. Files without markers are inlined verbatim — the
feature is fully opt-in per module.

---

## 7. Test: `make test`

```bash
make test [redis|none|all|<module> [<test_name>]] [TEST=<name>]
```

Dispatch:

| Command | Runs |
|---|---|
| `make test` | Redis tests only (`$(MAKE) -C src test`) |
| `make test redis` / `none` | Same — Redis tests only (mirrors `make build core`) |
| `make test all` / `.` | `make test` in every cloned module; continues past failures and summarizes at the end |
| `make test <module>` | `make test` in one module (full suite) |
| `make test <module> <test_name>` | `make test TEST=<test_name>` in one module |
| `make test <module> TEST=<name>` | Same, but for test names containing `:` (see below) |

### The `:` gotcha

GNU Make cannot have explicit target names containing `:` — the colon is
reserved for rule syntax. Test filters that include a `:` (common
convention: `file:test`, e.g. `test_asm:test_asm_with_data…`,
`test_basic.py:test_json_get`) **cannot** be passed positionally:

```bash
# Does NOT work — fails at parse time with a clear error:
make test redistimeseries test_asm:test_asm_with_data_and_queries_during_migrations

# Use TEST=<name> instead:
make test redistimeseries TEST=test_asm:test_asm_with_data_and_queries_during_migrations
```

Shell quotes (`"…"`, `'…'`) do **not** help — quoting is a shell concern,
not a Make one. The rule is:

| Arg shape | Make treats it as | May contain `:` |
|---|---|---|
| `foo` | Goal | No |
| `foo:bar` | Goal | No → error |
| `TEST=foo:bar` | Variable assignment | Yes |

### How test filtering is forwarded

Every cloned module (redisbloom, redisearch, redisjson, redistimeseries)
honors `TEST=<name>` in its own Makefile and forwards it to its test
runner (typically RLTest or pytest). Our `make test` simply sets that
variable on the sub-make invocation.

Examples:

```bash
make test                                          # Redis TCL tests
make test all                                      # every module's full suite
make test redistimeseries                          # redistimeseries full suite
make test redistimeseries ts_info                  # single simple-named test
make test redistimeseries TEST='test_asm:test_asm_with_data_and_queries_during_migrations'
make test redisjson       TEST='test_basic.py:test_json_get'
```

### `all` mode semantics

`make test all` runs each module's tests sequentially, **continuing on
failure** (unlike `make`, which fails fast). At the end it prints
a summary of which modules failed and exits non-zero if any did. This
matches typical test-runner expectations — you see every module's
results in one go.

Single-module invocations `exec` the sub-make, so the module's exit
code propagates directly.

---

## 8. End-to-end typical flow

```bash
# First time:
make modules-update                           # clone modules at pinned refs
make bootstrap                                     # install per-module deps (per-OS install)
make build                                    # build Redis, then every module

# Iterate:
make modules-update redisbloom                # bump to the current pin (re-runs are safe)
make build                                    # rebuild
make run redistimeseries redisbloom           # start Redis with just these two

# Verify:
redis-cli MODULE LIST

# Test:
make test                                     # Redis-only
make test redistimeseries                     # one module
make test redistimeseries TEST=ts_info        # one test
make test all                                 # every module
```

---

## 9. Release tarball: `make tarball`

Build a self-contained, reproducible source release tarball of Redis +
every module pinned in `modules.yaml`. The output is "ready to build" —
no `make modules-update` step required by the consumer.

```bash
make tarball TAG=<tag> \
                     [STAGING_DIR=<dir>] \
                     [OUT_PATH=<path>] \
                     [TAR=<gnu-tar>]
```

| Variable | Default | Purpose |
|---|---|---|
| `TAG` | *(required)* | Git ref of Redis core to archive (e.g. `8.0.0`) |
| `STAGING_DIR` | `/tmp/redis-tarball-staging-<tag>` | Workdir, wiped on entry and on success |
| `OUT_PATH` | `/tmp/redis-<tag>.tar.gz` | Final tarball location |
| `TAR` | `gtar` if found, else `tar` | Must be GNU tar (macOS BSD tar lacks `--sort`/`--mtime`) |

### What it does

1. Validates `TAG` resolves to a commit and that `TAR` is GNU tar.
2. `git archive <tag>` of Redis core into `<staging>/redis-<tag>/`.
3. For each module in `modules.yaml`, clones the upstream repo at the
   pinned `ref:` (kind resolved via `git ls-remote`: tag > branch >
   commit) into `<staging>/redis-<tag>/modules/<name>/src/`, recursively
   initializing submodules.
4. Strips `.git/`, `.github/`, and `.gitmodules` from cloned modules
   (Redis core's own `.github/` is preserved — it came from `git
   archive` and isn't dev-clone metadata).
5. Overlays the local `scripts/sync-redis-conf.sh`, `scripts/lib/manifest.sh`,
   and `modules/modules.yaml` so tarball-time fixes ship without re-tagging
   Redis core, then **bakes the module config into the packaged `redis.conf`**
   at staging time — the Modules block is generated and appended (with active
   `loadmodule` lines even though the `.so`s aren't built yet). So the shipped
   `redis.conf` loads the bundled modules out of the box; `redis-full.conf` is
   not packed separately.
6. Produces a deterministic tarball: entries sorted by name, mtimes
   pinned to the tag's commit timestamp, owner/group `0`, gzip with
   `-n` (no embedded mtime). Two runs from the same `TAG` yield
   byte-identical output.
7. Prints final size and sha256.

### Tarball layout

```
redis-<tag>/
├── (full Redis core source: src/, deps/, redis.conf, Makefile,
│   modules/{Makefile,common.mk,manifest.mk,modules.yaml,MODULES.md},
│   modules/vector-sets/, scripts/{build,deps,run,test,
│   modules-update,modules-shallow,sync-redis-conf,tarball}.sh,
│   scripts/lib/manifest.sh, ...)
└── modules/
    ├── redisbloom/src/      (cloned, no .git)
    ├── redisearch/src/      (cloned + recursive submodules, no .git)
    ├── redisjson/src/       (cloned, no .git)
    └── redistimeseries/src/ (cloned, no .git)
```

### Consumer flow

```bash
tar xzf redis-<tag>.tar.gz
cd redis-<tag>
gmake INSTALL_RUST_TOOLCHAIN=yes
./src/redis-server redis.conf
```

The tarball's `redis.conf` already has the module config baked in (done by
`make tarball` at packaging time — see §6.1), so it loads the bundled modules
out of the box; no extra step. If you'd rather keep the module config in a
separate file, `gmake sync-redis-conf` regenerates `redis-full.conf` and you can
run `./src/redis-server redis-full.conf` instead.

No network access needed at build time — modules are already on disk.

---

## 10. Full command reference

```
make bootstrap [<name> ...|all|.]             # per-module OS deps + Python venv (re-run as needed)

make modules-update [<name> ...]             # idempotent: clones if missing, else updates to pin
make modules-shallow <name> [<name> ...]     # re-clone module(s) shallow (--depth 1) to reclaim disk
make sync-redis-conf [<name> ...] \          # rewrite redis-full.conf from redis.conf + module.confs
    [MODULES="<names>"] [ASSUME_BUILT=1]
# (baking modules into redis.conf is not a make target — it happens inside
#  `make tarball` at packaging time; see §6.1)

make [<name> ...|all|.|redis|core|none] [VAR=value ...]
make all|build [<name> ...|all|.|redis|core|none] [VAR=value ...]

make run [<name> ...] [ARGS="<redis-server args>"]

make test
make test redis | none                       # Redis tests only (explicit)
make test all | .
make test <module>
make test <module> <test_name>
make test <module> TEST=<name>               # required for names containing ':'

make tarball TAG=<tag> [STAGING_DIR=<dir>] [OUT_PATH=<path>] [TAR=<gnu-tar>]
```

All targets are declared `.PHONY` and can be freely combined with
make's standard flags (`-j`, `-n`, `-C`, `-e`, …). Variables set on
the command line propagate to all sub-makes via `MAKEFLAGS`.

Every top-level target is a thin Makefile recipe that shells out to
`scripts/<name>.sh`. The scripts share `scripts/lib/manifest.sh` for
manifest reading; everything else is local to each script. See each
script's header for the full env-var contract.
