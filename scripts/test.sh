#!/bin/sh
# test.sh — dispatch test execution.
#
# Usage:  scripts/test.sh                            # Redis tests (make -C src test)
#         scripts/test.sh redis | none               # same — Redis only
#         scripts/test.sh all | . | '*'              # tests for every cloned module
#         scripts/test.sh <module>                   # full suite for one module
#         scripts/test.sh <module> <test_name>       # one test (positional)
# Env:    TEST=<name>   single-test selector for names containing ':' (which
#                       Make can't pass positionally). Positional <test_name>
#                       wins if both are given.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"
test_var="${TEST:-}"

if [ "$#" -eq 0 ]; then
  echo "==> Running Redis tests (src/)"
  exec "$MAKE_BIN" -C src test
fi

target="$1"; shift
cloned="$(cloned_modules)"

case "$target" in
  redis|none)
    if [ "$#" -gt 0 ] || [ -n "$test_var" ]; then
      echo "ERROR: cannot pass a test name together with '$target'"
      echo "       ('$target' selects Redis tests only — no module test name applies)"
      exit 1
    fi
    echo "==> Running Redis tests (src/)"
    exec "$MAKE_BIN" -C src test
    ;;
  all|.|'*')
    if [ "$#" -gt 0 ] || [ -n "$test_var" ]; then
      echo "ERROR: cannot pass a test name together with '$target'"
      echo "       (select a single module when running one test)"
      exit 1
    fi
    if [ -z "$cloned" ]; then
      echo "ERROR: no cloned modules under modules/*/src"
      echo "       run 'make modules-update all' and 'make build' first"
      exit 1
    fi
    echo "==> Running tests for all cloned modules: $cloned"
    failed=""
    for name in $cloned; do
      echo
      echo "==> [test] $name (modules/$name/src)"
      if ! "$MAKE_BIN" -C "modules/$name/src" test \
          REDIS_SERVER="$REPO_ROOT/src/redis-server"; then
        failed="$failed $name"
      fi
    done
    echo
    if [ -n "$failed" ]; then
      echo "==> Module tests FAILED for:$failed"
      exit 1
    fi
    echo "==> Module tests passed for all cloned modules"
    ;;
  *)
    ok=""
    case " $cloned " in *" $target "*) ok=1 ;; esac
    if [ -z "$ok" ]; then
      echo "ERROR: module '$target' is not cloned under modules/$target/src"
      echo "Cloned modules: $cloned"
      echo "Usage:"
      echo "  make test                             # run Redis tests"
      echo "  make test redis                       # run Redis tests (explicit)"
      echo "  make test all                         # run tests for every cloned module"
      echo "  make test <module>                    # run all tests for one module"
      echo "  make test <module> <test_name>        # run a single test (positional)"
      echo "  make test <module> TEST=<name>        # run a single test (use this for names with ':')"
      exit 1
    fi
    tname=""
    if [ "$#" -eq 1 ]; then
      tname="$1"
    elif [ "$#" -gt 1 ]; then
      echo "ERROR: too many arguments."
      echo "Usage: make test <module> [<test_name>] [TEST=<name>]"
      exit 1
    fi
    [ -z "$tname" ] && tname="$test_var"
    if [ -z "$tname" ]; then
      echo "==> Running all tests for module '$target'"
      exec "$MAKE_BIN" -C "modules/$target/src" test \
          REDIS_SERVER="$REPO_ROOT/src/redis-server"
    else
      echo "==> Running test '$tname' for module '$target' (TEST=$tname)"
      exec "$MAKE_BIN" -C "modules/$target/src" test TEST="$tname" \
          REDIS_SERVER="$REPO_ROOT/src/redis-server"
    fi
    ;;
esac
