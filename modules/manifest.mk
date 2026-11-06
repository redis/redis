# modules/manifest.mk — Make-side API for reading modules.yaml.
#
# All YAML parsing lives in scripts/lib/manifest.sh (single source of truth).
# This file is a thin Make wrapper that:
#   - resolves the manifest path so $(shell ...) calls work from any CWD
#     (top-level Makefile, per-module Makefile via common.mk, etc.),
#   - exposes the data Make needs at PARSE time (AVAILABLE_MODULES),
#   - exposes the helpers per-module Makefiles use during recipe expansion
#     ($(call manifest-field,<field>,<name>), $(call manifest-ref,<name>),
#      $(call manifest-ref-kind,<name>)).
#
# Why a Make file at all if the parsing lives in a shell script? Because
# `modules/common.mk` calls $(call manifest-field,...) at parse time to
# populate per-module variables (MODULE_REPO, MODULE_REF, etc.), and Make
# has no way to source a shell library — it can only consume strings from
# `$(shell ...)` invocations. So this file translates the shell library
# into Make functions.
#
# Each helper forks sh → manifest.sh → awk once per call. The manifest
# is tiny (<1 KB) and helpers are only invoked at parse time, not in
# build hot loops, so the per-call cost is invisible in practice.
#
# What this file provides:
#   MODULES_MANIFEST_FILE  - path to the YAML manifest, resolved from this
#                            file's location so callers don't have to set it.
#   AVAILABLE_MODULES      - sorted, space-separated list of module names.
#   $(call manifest-field,<field>,<name>)
#                          - one field for one module ("" if missing).
#   $(call manifest-ref,<name>)
#                          - the module's `ref:` value verbatim.
#   $(call manifest-ref-kind,<name>)
#                          - one of: tag | branch | commit, resolved against
#                            the upstream `repo:` via `git ls-remote`
#                            (priority: tag > branch > commit).
#
# YAML format the parser accepts is documented at the top of
# scripts/lib/manifest.sh.

# Locate this .mk file's directory; everything else is computed from it so
# resolution works whether we were included by the repo-root Makefile
# (CWD = repo root) or by a per-module Makefile via common.mk (CWD =
# modules/<name>). `$(lastword MAKEFILE_LIST)` is set to this file's path
# at the moment Make is processing it, so the resulting paths are
# correctly relative to the per-include CWD.
MANIFEST_MK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
MODULES_MANIFEST_FILE ?= $(MANIFEST_MK_DIR)modules.yaml
MANIFEST_SH           := $(MANIFEST_MK_DIR)../scripts/lib/manifest.sh

# Fail loudly when the manifest is missing — better than silently collapsing
# to an empty $(AVAILABLE_MODULES) and then no-op-ing every downstream target.
ifeq ($(wildcard $(MODULES_MANIFEST_FILE)),)
  $(error modules.yaml not found at $(MODULES_MANIFEST_FILE))
endif

# Common prefix for every $(shell ...) below. Passing MODULES_MANIFEST_FILE
# explicitly means the script uses Make's resolution of the path (which
# honors per-module CWD offsets) instead of recomputing its own from
# REPO_ROOT.
_MANIFEST_SH := MODULES_MANIFEST_FILE='$(MODULES_MANIFEST_FILE)' $(MANIFEST_SH)

# Sorted list of every module known to the manifest. Computed once at parse
# time. Empty if the manifest is empty or unreadable; downstream targets
# already handle that case.
AVAILABLE_MODULES := $(shell $(_MANIFEST_SH) modules 2>/dev/null)

# Recursive (=) so each call re-invokes the script with the current args.
# Argument order matches the original Make-internal signatures:
#   $(call manifest-field,<field>,<name>)  — note: field first, name second
#   $(call manifest-ref,<name>)
#   $(call manifest-ref-kind,<name>)
# The script's CLI is the other way around (`field <name> <field>`), to
# match the underlying shell-function signatures; the wrapper just swaps.
manifest-field    = $(shell $(_MANIFEST_SH) field "$2" "$1" 2>/dev/null)
manifest-ref      = $(shell $(_MANIFEST_SH) ref "$1" 2>/dev/null)
manifest-ref-kind = $(shell $(_MANIFEST_SH) ref-kind "$1" 2>/dev/null)

# Note: this file intentionally defines NO targets.
#
# Both the top-level Makefile and modules/common.mk (via every per-module
# Makefile) include manifest.mk to read the manifest. If we declared a
# target here, it would become the default goal of any per-module
# `make -C modules/<name>` invocation (since GNU Make uses the first
# non-pattern target it sees). Repo-wide targets like `sync-redis-conf`
# live in the top-level Makefile so per-module builds don't see them.
