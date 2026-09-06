#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 /path/to/redis-server /path/to/redis-cli" >&2
    exit 2
fi

server_bin=$(realpath "$1")
cli_bin=$(realpath "$2")
containers=${BITMAP_BENCH_CONTAINERS:-1000000}
bench_dir=$(mktemp -d)
pidfile="$bench_dir/redis.pid"
logfile="$bench_dir/redis.log"
socket="$bench_dir/redis.sock"

cleanup() {
    if [[ -f "$pidfile" ]]; then
        pid=$(<"$pidfile")
        if ! "$cli_bin" -s "$socket" shutdown nosave >/dev/null 2>&1; then
            kill "$pid" >/dev/null 2>&1 || true
        fi
    fi
    rm -rf "$bench_dir"
}
trap cleanup EXIT

"$server_bin" \
    --port 0 \
    --unixsocket "$socket" \
    --unixsocketperm 700 \
    --save '' \
    --appendonly no \
    --daemonize yes \
    --dir "$bench_dir" \
    --pidfile "$pidfile" \
    --logfile "$logfile" \
    --bitmap-default-roaring yes \
    --proto-max-bulk-len 16gb \
    --slowlog-log-slower-than 0

ready=0
for _ in $(seq 1 100); do
    if [[ -f "$pidfile" ]] && "$cli_bin" -s "$socket" ping >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.05
done
if (( ready == 0 )); then
    cat "$logfile" >&2
    exit 1
fi

# One bit in every 2^16-bit range creates one compact ARRAY container. Inline
# protocol keeps fixture creation out of the duration being measured.
seq 0 $((containers - 1)) |
    awk '{printf "SETBIT bitmap:container-count %.0f 1\n", $1 * 65536}' |
    "$cli_bin" -s "$socket" --pipe >/dev/null

memory_bytes=$("$cli_bin" -s "$socket" memory usage bitmap:container-count)
"$cli_bin" -s "$socket" slowlog reset >/dev/null
"$cli_bin" -s "$socket" unlink bitmap:container-count >/dev/null

# SLOWLOG GET itself is recorded only after its reply is produced, so the first
# entry returned here is the immediately preceding UNLINK. Its third RESP item
# is command duration in microseconds.
duration_us=$("$cli_bin" -s "$socket" --raw slowlog get 1 | sed -n '3p')

if [[ ! "$duration_us" =~ ^[0-9]+$ ]]; then
    echo "unable to read UNLINK duration from SLOWLOG" >&2
    exit 1
fi

printf 'containers=%s memory_bytes=%s duration_us=%s\n' \
    "$containers" "$memory_bytes" "$duration_us"
