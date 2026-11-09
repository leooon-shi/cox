#!/usr/bin/env bash
# sample.sh: the containment sampler for load-validation (docs/insights/features/
# load-validation-design.md). Once per second it records, into a CSV, the three
# signals the pass/fail checks need:
#   - from the server's /metrics: live connections, accepted/closed/rejected
#     totals (the Cox-reported view of what the defense is doing);
#   - from /proc/<pid>/status: VmRSS (memory containment -- does it plateau or
#     grow without bound?);
#   - from ss: open socket count toward the server port (fd containment,
#     cross-checking the metrics' live count against the kernel's view).
#
# It is a glue tool on purpose (curl + /proc + ss), so it is a script, not a
# binary -- trivially adjustable per scenario. Standalone like the other load
# tools; no Cox dependency.
#
# Usage:
#   sample.sh --pid PID --metrics-url URL [--port N] [--count N]
#             [--interval S] [--out FILE]
#
#   --pid PID       server process id (for /proc/<pid>/status VmRSS). Required.
#   --metrics-url   e.g. http://127.0.0.1:19101/metrics. Required.
#   --port N        data port, for the ss socket count (default: parsed from
#                   nothing -- if omitted, the ss column is left empty).
#   --count N       number of samples (0 = until killed; default 0).
#   --interval S    seconds between samples (default 1).
#   --out FILE      CSV output path (default: stdout).
#
# CSV columns:
#   t_s,rss_kib,sockets,live,accepted,closed,rejected
# where t_s is seconds since the sampler started.

set -u

pid=""
metrics_url=""
port=""
count=0
interval=1
out=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) pid="$2"; shift 2 ;;
        --metrics-url) metrics_url="$2"; shift 2 ;;
        --port) port="$2"; shift 2 ;;
        --count) count="$2"; shift 2 ;;
        --interval) interval="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$pid" || -z "$metrics_url" ]]; then
    echo "usage: $0 --pid PID --metrics-url URL [--port N] [--count N] [--interval S] [--out FILE]" >&2
    exit 2
fi

# Resolve the output sink once.
emit() {
    if [[ -n "$out" ]]; then
        printf '%s\n' "$1" >>"$out"
    else
        printf '%s\n' "$1"
    fi
}

# Truncate/overwrite the CSV and write the header.
if [[ -n "$out" ]]; then
    : >"$out"
fi
emit "t_s,rss_kib,sockets,live,accepted,closed,rejected"

# VmRSS in KiB from /proc/<pid>/status, or empty if the process is gone.
read_rss() {
    local v
    v=$(awk '/^VmRSS:/{print $2}' "/proc/$1/status" 2>/dev/null)
    printf '%s' "${v:-}"
}

# Count established+listening sockets toward the data port (kernel's view of
# fd/connection pressure). Empty if --port not given or ss unavailable.
read_sockets() {
    if [[ -z "$port" ]] || ! command -v ss >/dev/null 2>&1; then
        printf ''
        return
    fi
    # Match ":<port> " in either local or peer address; -H omits the header,
    # -t TCP only, -n numeric. Counting lines is a close-enough proxy for the
    # socket pressure the server is under.
    ss -Htn "sport = :$port or dport = :$port" 2>/dev/null | wc -l | tr -d ' '
}

# Pull a single metric's scalar value from the Prometheus text (first match of
# a bare "<name> <value>" line; ignores HELP/TYPE comment lines).
metric_of() {
    awk -v n="$1" '$1==n {print $2; exit}'
}

start=$(date +%s.%N)
i=0
while :; do
    now=$(date +%s.%N)
    t_s=$(awk -v a="$now" -v b="$start" 'BEGIN{printf "%.1f", a-b}')

    rss=$(read_rss "$pid")
    sockets=$(read_sockets)

    # One scrape, parsed for the four series the scenarios care about.
    scrape=$(curl -s --max-time 2 "$metrics_url" 2>/dev/null)
    live=$(printf '%s' "$scrape" | metric_of cox_connections_live)
    accepted=$(printf '%s' "$scrape" | metric_of cox_connections_accepted_total)
    closed=$(printf '%s' "$scrape" | metric_of cox_connections_closed_total)
    rejected=$(printf '%s' "$scrape" | metric_of cox_connections_rejected_total)

    emit "${t_s},${rss},${sockets},${live:-},${accepted:-},${closed:-},${rejected:-}"

    i=$((i + 1))
    if [[ "$count" -ne 0 && "$i" -ge "$count" ]]; then
        break
    fi
    # Stop early if the server process has exited.
    if [[ ! -d "/proc/$pid" ]]; then
        break
    fi
    sleep "$interval"
done
