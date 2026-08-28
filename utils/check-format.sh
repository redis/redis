#!/bin/sh
# Check C and header lines changed from a base revision.
#
#   make format                         # current branch vs. unstable
#   make format FIX=1                   # apply fixes instead of reporting them
#   make format BASE=HEAD               # working tree vs. HEAD

set -eu

BASE=${1:-unstable}

CF=${CLANG_FORMAT:-clang-format}
GCF=${GIT_CLANG_FORMAT:-git-clang-format}

if [ "$#" -gt 1 ]; then
    echo "Usage: $0 [<base>]" >&2
    exit 2
fi

ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "check-format.sh must run inside a Git repository." >&2
    exit 2
}
cd "$ROOT"

command -v "$CF" >/dev/null 2>&1 || {
    echo "clang-format not found. Install it with: sudo apt-get install clang-format" >&2
    exit 2
}
command -v "$GCF" >/dev/null 2>&1 || {
    echo "git-clang-format not found. Install it with: sudo apt-get install clang-format" >&2
    exit 2
}
[ -f .clang-format ] || {
    echo ".clang-format is required at the repository root." >&2
    exit 2
}
git rev-parse --verify "${BASE}^{commit}" >/dev/null 2>&1 || {
    echo "Base $BASE does not resolve to a commit." >&2
    exit 2
}
DIFF_BASE=$(git merge-base "$BASE" HEAD) || {
    echo "Could not find a merge base for $BASE and HEAD." >&2
    exit 2
}

if [ -n "${FIX:-}" ]; then
    status=0
    "$GCF" --binary "$CF" --extensions c,h --style file --force \
        "$DIFF_BASE" -- ':!deps/**' || status=$?
    case "$status" in
        0|1) ;;
        *) exit "$status" ;;
    esac
    FIX= "$0" "$BASE"
    exit $?
fi

status=0
out=$("$GCF" --binary "$CF" --extensions c,h --style file --diff \
    "$DIFF_BASE" -- ':!deps/**' 2>&1) || status=$?

case "$status" in
    0)
        echo "Format OK."
        exit 0
        ;;
    1)
        printf '%s\n\n' "$out"
        echo "The lines above are not formatted as expected."
        echo "Run 'make format FIX=1' to fix them."
        exit 1
        ;;
    *)
        printf '%s\n' "$out" >&2
        exit "$status"
        ;;
esac
