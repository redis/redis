# Top level makefile, the real stuff is at ./src/Makefile and in ./modules/Makefile

# Explicitly export $(MAKE) so recipe shells (scripts/build.sh, deploy.sh, ...)
# can read it as $MAKE and use the same make binary the user invoked. GNU make
# only auto-propagates MAKE to recursive `$(MAKE) -C ...` sub-makes — recipes
# that are shell scripts get an EMPTY $MAKE env var by default. Without this,
# `MAKE_BIN="${MAKE:-make}"` in scripts falls back to PATH-lookup of `make`,
# which can resolve to Apple's stock /usr/bin/make 3.81 in non-interactive
# recipe shells (different PATH ordering than the user's interactive shell).
export MAKE

SUBDIRS = src
ifeq ($(BUILD_WITH_MODULES), yes)
	ifeq ($(MAKECMDGOALS),32bit)
    	$(error BUILD_WITH_MODULES=yes is not supported on 32 bit systems)
	endif
	SUBDIRS += modules
endif

# Manifest parser (modules.yaml → AVAILABLE_MODULES + helpers) shared with
# per-module builds via modules/common.mk. The `sync-redis-conf` target lives
# in this top-level Makefile (not in manifest.mk) so per-module builds —
# which include common.mk → manifest.mk — don't see it as their default goal.
include modules/manifest.mk

# ----------------------------------------------------------------------------
# Positional-arg capture for goals that take a list of module/test names.
#
# `make modules-update redistimeseries redisjson` is parsed by Make as three
# goals; we want only `modules-update` to run, with the rest captured into
# MODULES_ARGS. Each entry in GOALS_WITH_ARGS is `<goal>:<VAR>`. To add a
# new positional-arg goal, append it here — the dispatch and no-op .PHONY
# targets fall out automatically.
# ----------------------------------------------------------------------------
GOALS_WITH_ARGS := \
    modules-update:MODULES_ARGS \
    modules-shallow:SHALLOW_ARGS \
    run:RUN_ARGS \
    clean:CLEAN_ARGS \
    bootstrap:BOOTSTRAP_ARGS \
    deploy:DEPLOY_ARGS \
    test:TEST_ARGS \
    sync-redis-conf:SYNC_ARGS \
    apply-redis-conf:APPLY_ARGS

# When <goal> is the top-level goal, stash trailing positional args into <var>
# and turn each non-':'-bearing token into a no-op .PHONY target so .DEFAULT
# below doesn't recurse into $(SUBDIRS) for them. Tokens containing ':' are
# skipped here (they'd crash Make's static-pattern parser); goals that want to
# forbid them validate in a follow-up block.
define _capture_goal_args
ifeq ($$(firstword $$(MAKECMDGOALS)),$(1))
  $(2) := $$(filter-out $(1),$$(MAKECMDGOALS))
  $$(foreach m,$$($(2)),$$(if $$(findstring :,$$(m)),,$$(eval .PHONY: $$(m))$$(eval $$(m): ; @:)))
endif
endef

$(foreach pair,$(GOALS_WITH_ARGS), \
  $(eval $(call _capture_goal_args,$(firstword $(subst :, ,$(pair))),$(lastword $(subst :, ,$(pair))))))

# `test` extra validation: positional names containing ':' (e.g. `file:test`)
# would fail Make parsing, so force them through TEST=<name>.
ifeq ($(firstword $(MAKECMDGOALS)),test)
  TEST_ARGS_BAD := $(strip $(foreach m,$(TEST_ARGS),$(if $(findstring :,$(m)),$(m))))
  ifneq ($(TEST_ARGS_BAD),)
    $(error Test name(s) containing ':' cannot be passed positionally to make: '$(TEST_ARGS_BAD)'. Use TEST=<name> instead, e.g. `$(MAKE) test redistimeseries TEST='$(firstword $(TEST_ARGS_BAD))'`)
  endif
endif

# `sync-redis-conf <name> ...` glue: feed positional args into MODULES (which
# the recipe below passes to scripts/sync-redis-conf.sh). Recursive invocations
# from build/modules-update pass MODULES= explicitly, so SYNC_ARGS stays empty
# there.
ifeq ($(firstword $(MAKECMDGOALS)),sync-redis-conf)
  ifneq ($(SYNC_ARGS),)
    MODULES := $(SYNC_ARGS)
  endif
endif

# Same glue for `apply-redis-conf <name> ...` — forwards positional args
# into MODULES so the underlying script sees the subset. `revert` is
# special: it stays in APPLY_ARGS (not in MODULES) so the script sees it
# as the mode toggle. All other positional tokens flow into MODULES.
ifeq ($(firstword $(MAKECMDGOALS)),apply-redis-conf)
  ifneq ($(APPLY_ARGS),)
    MODULES := $(filter-out revert,$(APPLY_ARGS))
  endif
endif

.DEFAULT:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

install:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

# clean [<name> ...|all|.|'*'|redis|none] — Redis core + selected modules.
# Same per-module dispatch as scripts/build.sh. Env vars set on the make
# line (e.g. `make clean DEPS=1`) propagate to the per-module clean via
# the shell environment.
clean:
	@scripts/clean.sh $(CLEAN_ARGS)

# ----------------------------------------------------------------------------
# Module / build / test orchestration. Recipes are thin wrappers around
# scripts/ so the logic stays out of Make. See each script's header for full
# usage. All scripts respect $(MAKE) and run from the repo root.
# ----------------------------------------------------------------------------

# all / make: Redis core + all cloned modules.
# BUILD_WITH_MODULES=yes is accepted for backward compatibility — same result.
all:
	@scripts/build.sh

# core: Redis core only (no modules).
core:
	@scripts/build.sh redis

# Per-module targets: `make redistimeseries` → Redis core + that module.
define _module_build_target
$(1):
	@scripts/build.sh $(1)
endef
$(foreach m,$(AVAILABLE_MODULES),$(eval $(call _module_build_target,$(m))))

# bootstrap [<name> ...|all|.|'*'] — install per-module build/test prereqs.
bootstrap:
	@scripts/bootstrap.sh $(BOOTSTRAP_ARGS)

# deploy [<name> ...|all|.|'*'|redis|none] [PREFIX=<path>] [DESTDIR=<path>]
#   Install Redis core + selected modules (default: every cloned module),
#   then auto-rewrite redis.conf so its `loadmodule` lines point at the
#   installed .so paths. PREFIX defaults to /usr/local (same as `make install`).
#   apply-redis-conf is called with PREFIX=$(PREFIX)/lib/redis/modules — the
#   actual modules directory `make install` / `make deploy` write to.
deploy: PREFIX ?= /usr/local
deploy:
	@PREFIX='$(PREFIX)' DESTDIR='$(DESTDIR)' scripts/deploy.sh $(DEPLOY_ARGS)
	@echo
	@echo "==> Updating redis.conf for installed layout (PREFIX=$(PREFIX)/lib/redis/modules)"
	@$(MAKE) --no-print-directory apply-redis-conf \
	    PREFIX='$(PREFIX)/lib/redis/modules' \
	    MODULES='$(DEPLOY_ARGS)'

# run [<name> ...|all|.|'*'|none] [ARGS="<redis-server flags>"]
run:
	@ARGS='$(ARGS)' scripts/run.sh $(RUN_ARGS)

# test [redis|all|<module> [<test_name>]] [TEST=<name>] — see scripts/test.sh.
test:
	@TEST='$(TEST)' scripts/test.sh $(TEST_ARGS)

# modules-update [<name> ...|all|.|'*'] [MODULES_UPDATE_SHALLOW=1]
#   Idempotent clone/refresh per modules.yaml.
modules-update:
	@MODULES_UPDATE_SHALLOW='$(MODULES_UPDATE_SHALLOW)' scripts/modules-update.sh $(MODULES_ARGS)

# modules-shallow <name> [<name> ...] — re-clone selected modules with --depth 1.
modules-shallow:
	@scripts/modules-shallow.sh $(SHALLOW_ARGS)

# tarball TAG=<ref> [STAGING_DIR=...] [OUT_PATH=...] [TAR=...] [TARBALL_SKIP_MODULES_UPDATE=1]
#   Reproducible Redis+modules source tarball.
tarball:
	@TAG='$(TAG)' STAGING_DIR='$(STAGING_DIR)' OUT_PATH='$(OUT_PATH)' \
	    TAR='$(TAR)' TARBALL_SKIP_MODULES_UPDATE='$(TARBALL_SKIP_MODULES_UPDATE)' \
	    scripts/tarball.sh

# sync-redis-conf [<name> ...] [MODULES="<names>"] [ASSUME_BUILT=1|yes|true]
#   Rewrite the untracked redis-full.conf from redis.conf + modules.yaml +
#   per-module module.conf files. See scripts/sync-redis-conf.sh for the full
#   contract (env vars, file layout, private-block stripping).
sync-redis-conf:
	@REDIS_CONF='$(REDIS_CONF)' REDIS_GEN_CONF='$(REDIS_GEN_CONF)' \
	    MODULES='$(strip $(MODULES))' ASSUME_BUILT='$(strip $(ASSUME_BUILT))' \
	    MODULES_MANIFEST_FILE='$(MODULES_MANIFEST_FILE)' \
	    PREFIX='$(PREFIX)' \
	    scripts/sync-redis-conf.sh

# apply-redis-conf [<name> ...|revert] [MODULES="<names>"] [ASSUME_BUILT=1|yes|true]
#   Default mode: run sync-redis-conf, then OVERWRITE redis.conf with the
#   generated content. Destructive on the tracked redis.conf — use
#   `make apply-redis-conf revert` to strip just the appended Modules section
#   back out, or `git checkout -- redis.conf` to revert everything.
#   Idempotent: re-running is safe because sync extracts only the Redis-core
#   section between the BEGIN/END markers in redis.conf. Typical use: after
#   extracting a release tarball + `make`, so
#   `./src/redis-server redis.conf` auto-loads bundled modules.
apply-redis-conf:
	@REDIS_CONF='$(REDIS_CONF)' REDIS_GEN_CONF='$(REDIS_GEN_CONF)' \
	    MODULES='$(strip $(MODULES))' ASSUME_BUILT='$(strip $(ASSUME_BUILT))' \
	    MODULES_MANIFEST_FILE='$(MODULES_MANIFEST_FILE)' \
	    PREFIX='$(PREFIX)' \
	    scripts/apply-redis-conf.sh $(filter revert,$(APPLY_ARGS))

.PHONY: all core install clean run test bootstrap deploy modules-update modules-shallow sync-redis-conf apply-redis-conf tarball $(AVAILABLE_MODULES)
