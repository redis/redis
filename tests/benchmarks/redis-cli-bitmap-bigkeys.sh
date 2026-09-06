#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 /path/to/redis-server /path/to/redis-cli" >&2
    exit 2
fi

server_bin=$(realpath "$1")
cli_bin=$(realpath "$2")
keys=${BITMAP_BENCH_KEYS:-20000}
shape_keys=${BITMAP_BENCH_SHAPE_KEYS:-1000}
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
    --bitmap-default-roaring yes

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

# Populate one-bit native bitmaps, give a subset 32 sparse containers, then
# make the final key eight bits wide. Fixture creation is outside the measured
# region. The sparse subset catches BITCOUNT cost that scales with containers.
{
    seq 1 "$keys" | awk '{printf "SETBIT bitmap:%d 0 1\r\n", $1}'
    seq 1 "$shape_keys" | awk '{
        for (container = 1; container < 32; container++)
            printf "SETBIT bitmap:%d %d 1\r\n", $1, container * 65536
    }'
    for bit in $(seq 1 7); do
        printf 'SETBIT bitmap:%s %s 1\r\n' "$keys" "$bit"
    done
} |
    "$cli_bin" -s "$socket" --pipe >/dev/null

start_ns=$(date +%s%N)
output=$("$cli_bin" -s "$socket" --bigkeys)
end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

summary=$(awk '$2 == "bitmaps" && $3 == "with" {
    unit = $5;
    if ($5 == "set") unit = $5 "_" $6;
    print $1, $4, unit;
}' <<<"$output")
read -r bitmap_count bitmap_total unit <<<"$summary"

if [[ ! "$bitmap_count" =~ ^[0-9]+$ || ! "$bitmap_total" =~ ^[0-9]+$ || -z "$unit" ]]; then
    echo "unable to parse bitmap summary from redis-cli --bigkeys" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

expected_total=$((keys + shape_keys * 31 + 7))
printf 'elapsed_ms=%s bitmap_count=%s bitmap_total=%s unit=%s expected_total=%s\n' \
    "$elapsed_ms" "$bitmap_count" "$bitmap_total" "$unit" "$expected_total"
