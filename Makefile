# Top level makefile, the real stuff is at ./src/Makefile and in ./modules/Makefile

SUBDIRS = src
ifeq ($(BUILD_WITH_MODULES), yes)
	ifeq ($(MAKECMDGOALS),32bit)
    	$(error BUILD_WITH_MODULES=yes is not supported on 32 bit systems)
	endif
	SUBDIRS += modules
endif

default: all

# Manifest parser (modules.yaml → AVAILABLE_MODULES + helpers) shared with
# per-module builds via modules/common.mk. The `sync-redis-conf` target lives
# in this top-level Makefile (not in manifest.mk) so per-module builds —
# which include common.mk → manifest.mk — don't see it as their default goal.
include modules/manifest.mk

# ----------------------------------------------------------------------------
# Positional-arg capture for goals that take a list of module/test names.
#
# `make build redistimeseries redisjson` is parsed by Make as three goals; we
# want only `build` to run, with the rest captured into BUILD_ARGS. Each entry
# in GOALS_WITH_ARGS is `<goal>:<VAR>`. To add a new positional-arg goal,
# append it here — the dispatch and no-op .PHONY targets fall out automatically.
# ----------------------------------------------------------------------------
GOALS_WITH_ARGS := \
    modules-update:MODULES_ARGS \
    modules-shallow:SHALLOW_ARGS \
    run:RUN_ARGS \
    build:BUILD_ARGS \
    bootstrap:BOOTSTRAP_ARGS \
    setup:SETUP_ARGS \
    test:TEST_ARGS \
    sync-redis-conf:SYNC_ARGS

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

.DEFAULT:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

install:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

# ----------------------------------------------------------------------------
# Module / build / test orchestration. Recipes are thin wrappers around
# scripts/ so the logic stays out of Make. See each script's header for full
# usage. All scripts respect $(MAKE) and run from the repo root.
# ----------------------------------------------------------------------------

# build [<name> ...|all|.|'*'|redis|none] — Redis core + selected modules.
build:
	@scripts/build.sh $(BUILD_ARGS)

# bootstrap [<name> ...|all|.|'*'] — install per-module build/test prereqs.
bootstrap:
	@scripts/bootstrap.sh $(BOOTSTRAP_ARGS)

# setup [<name> ...|all|.|'*'] — modules-update + bootstrap in one step.
setup:
	@scripts/setup.sh $(SETUP_ARGS)

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
#   Rewrite the untracked redis-gen.conf from redis.conf + modules.yaml +
#   per-module module.conf files. See scripts/sync-redis-conf.sh for the full
#   contract (env vars, file layout, private-block stripping).
sync-redis-conf:
	@REDIS_CONF='$(REDIS_CONF)' REDIS_GEN_CONF='$(REDIS_GEN_CONF)' \
	    MODULES='$(strip $(MODULES))' ASSUME_BUILT='$(strip $(ASSUME_BUILT))' \
	    MODULES_MANIFEST_FILE='$(MODULES_MANIFEST_FILE)' \
	    scripts/sync-redis-conf.sh

.PHONY: install build run test setup bootstrap modules-update modules-shallow sync-redis-conf tarball
