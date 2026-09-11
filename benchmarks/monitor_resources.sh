#!/usr/bin/env bash
# Samples a process's CPU% and RSS every INTERVAL seconds via /proc, for
# running alongside forgefs_benchmark to get server-side resource numbers
# the benchmark tool itself can't see (it only measures the client side of
# the wire). Linux only.
#
# Usage: monitor_resources.sh <pid> [interval_seconds] [output.csv]
#   monitor_resources.sh "$(pgrep -f forgefs_server)" 1 coordinator_usage.csv
#
# Caveat: /proc/<pid>/stat's comm field (2nd field, in parens) can contain
# spaces, which would shift naive whitespace-split field indices. This
# script is fine for forgefs_server/forgefs_storage specifically (neither
# name has spaces) but isn't a general-purpose /proc/stat parser.
set -euo pipefail

PID="${1:?usage: monitor_resources.sh <pid> [interval_seconds] [output.csv]}"
INTERVAL="${2:-1}"
OUT="${3:-/dev/stdout}"

if [ ! -d "/proc/$PID" ]; then
    echo "no such process: $PID" >&2
    exit 1
fi

HZ=$(getconf CLK_TCK)
echo "timestamp,cpu_percent,rss_kb" > "$OUT"

prev_total=0
prev_time=$(date +%s.%N)

while [ -d "/proc/$PID" ]; do
    read -r -a stat_fields < "/proc/$PID/stat" || break
    utime=${stat_fields[13]}
    stime=${stat_fields[14]}
    total=$((utime + stime))
    now=$(date +%s.%N)

    rss_kb=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)

    cpu_percent=$(awk -v t="$total" -v pt="$prev_total" -v hz="$HZ" -v now="$now" -v prev="$prev_time" \
        'BEGIN { elapsed = now - prev; if (elapsed <= 0) elapsed = 0.001; printf "%.2f", ((t - pt) / hz) / elapsed * 100 }')

    echo "$(date +%H:%M:%S),${cpu_percent},${rss_kb}" >> "$OUT"

    prev_total=$total
    prev_time=$now
    sleep "$INTERVAL"
done
