#!/usr/bin/env bash
# hardening_smoke.sh: a REDUCED, fast, containment-only smoke tier for the
# load-validation scenarios (docs/performance/hardening-validation.md).
#
# Philosophy (design doc §5): a full 20k-connection attack is noisy and
# meaningless on a shared CI runner, so this asserts only CONTAINMENT INVARIANTS
# that hold regardless of machine speed -- bounded live connections, a rejected/
# closed counter advancing, a gauge staying under its cap -- NEVER latency or
# CPU thresholds (which are hardware-dependent). It is wired into CI as a
# NON-BLOCKING job (like the benchmark/coverage jobs): a failure is a signal to
# look, not a hard gate, because socket timing on shared runners can be flaky.
#
# Scales are tiny (hundreds of connections, a few seconds) so the whole tier
# runs in well under a minute. Each check starts its own server on its own port,
# asserts, and tears down.
#
# Usage: hardening_smoke.sh [path-to-bin-dir]   (default build/examples/bin)

set -u

BIN="${1:-build/examples/bin}"
SERVER="$BIN/c10k_server"
HTTP="$BIN/http_target"
CLIENT="$BIN/c10k_client"

for exe in "$SERVER" "$HTTP" "$CLIENT"; do
    if [[ ! -x "$exe" ]]; then
        echo "FATAL: $exe not found/executable (build the 'examples' preset first)" >&2
        exit 2
    fi
done

pass=0
fail=0
# Base port; each check uses BASE+offset for data and BASE+100+offset for metrics
# to avoid collisions with a running test suite.
BASE=21100

# Scrape a single bare "<name> <value>" metric (optionally with a label filter).
# $1 metrics url, $2 metric line regex -> prints the value (numeric) or empty.
metric() {
    curl -s --max-time 2 "$1" 2>/dev/null | awk -v re="$2" '$0 ~ re {print $2; exit}'
}

ok()   { echo "  PASS: $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL: $1"; fail=$((fail+1)); }

# Wait until a server's /metrics endpoint answers (bounded), so we don't scrape
# before it is listening.
wait_metrics() {
    for _ in $(seq 1 50); do
        if curl -s --max-time 1 "$1" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    return 1
}

# ---- S5: flood vs max-conns/accept-rate -> live<=cap, rejected advances ----
check_s5() {
    echo "S5 flood vs max-conns/accept-rate:"
    local dp=$((BASE+5)) mp=$((BASE+105))
    "$SERVER" --port "$dp" --metrics-port "$mp" --max-conns 100 \
        --accept-qps 200 --quiet >/dev/null 2>&1 &
    local srv=$!
    wait_metrics "http://127.0.0.1:$mp/metrics" || { bad "S5 server did not start"; kill $srv 2>/dev/null; return; }
    "$CLIENT" --port "$dp" --conns 400 --attack flood --duration 3 >/dev/null 2>&1
    local live rej
    live=$(metric "http://127.0.0.1:$mp/metrics" '^cox_connections_live ')
    rej=$(metric "http://127.0.0.1:$mp/metrics" '^cox_connections_rejected_total ')
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    awk -v l="${live:-999999}" 'BEGIN{exit !(l<=100)}' && ok "live=${live:-?} <= cap 100" || bad "live=${live:-?} > cap 100"
    awk -v r="${rej:-0}" 'BEGIN{exit !(r>0)}' && ok "rejected=${rej:-0} advanced" || bad "rejected did not advance"
}

# ---- S6: max-age recycling -> closed{reason=max_age} advances ----
check_s6() {
    echo "S6 max-age recycling:"
    local dp=$((BASE+6)) mp=$((BASE+106))
    "$SERVER" --port "$dp" --metrics-port "$mp" --max-age 2 --quiet >/dev/null 2>&1 &
    local srv=$!
    wait_metrics "http://127.0.0.1:$mp/metrics" || { bad "S6 server did not start"; kill $srv 2>/dev/null; return; }
    "$CLIENT" --port "$dp" --conns 100 --msg 0 --duration 5 >/dev/null 2>&1
    local ma
    ma=$(metric "http://127.0.0.1:$mp/metrics" 'reason="max_age"')
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    awk -v m="${ma:-0}" 'BEGIN{exit !(m>0)}' && ok "closed{reason=max_age}=${ma:-0} advanced" || bad "max_age closes did not advance"
}

# ---- S2: write-timeout vs slow-read -> closed{reason=write_timeout} advances ----
check_s2() {
    echo "S2 write-timeout vs slow-read:"
    local dp=$((BASE+2)) mp=$((BASE+102))
    "$SERVER" --port "$dp" --metrics-port "$mp" --write-timeout 2 --quiet >/dev/null 2>&1 &
    local srv=$!
    wait_metrics "http://127.0.0.1:$mp/metrics" || { bad "S2 server did not start"; kill $srv 2>/dev/null; return; }
    # 16 MiB unread echo per conn overflows the socket buffer and backs up.
    "$CLIENT" --port "$dp" --conns 20 --attack slow-read --msg 16777216 --duration 5 >/dev/null 2>&1
    local wt
    wt=$(metric "http://127.0.0.1:$mp/metrics" 'reason="write_timeout"')
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    awk -v w="${wt:-0}" 'BEGIN{exit !(w>0)}' && ok "closed{reason=write_timeout}=${wt:-0} advanced" || bad "write_timeout closes did not advance"
}

# ---- S3: input-cap vs unframed flood -> closed{reason=input_cap} advances ----
check_s3() {
    echo "S3 input-cap vs unframed flood:"
    local dp=$((BASE+3)) mp=$((BASE+103))
    "$SERVER" --port "$dp" --metrics-port "$mp" --sink --max-input-buf 1048576 --quiet >/dev/null 2>&1 &
    local srv=$!
    wait_metrics "http://127.0.0.1:$mp/metrics" || { bad "S3 server did not start"; kill $srv 2>/dev/null; return; }
    "$CLIENT" --port "$dp" --conns 30 --attack slow-read --msg 67108864 --duration 4 >/dev/null 2>&1
    local ic
    ic=$(metric "http://127.0.0.1:$mp/metrics" 'reason="input_cap"')
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    awk -v i="${ic:-0}" 'BEGIN{exit !(i>0)}' && ok "closed{reason=input_cap}=${ic:-0} advanced" || bad "input_cap closes did not advance"
}

# ---- S4: HTTP body limit vs big-body -> http 4xx advances ----
check_s4() {
    echo "S4 HTTP body-limit vs big-body:"
    local dp=$((BASE+4)) mp=$((BASE+104))
    "$HTTP" --port "$dp" --metrics-port "$mp" --max-body 1048576 --quiet >/dev/null 2>&1 &
    local srv=$!
    wait_metrics "http://127.0.0.1:$mp/metrics" || { bad "S4 server did not start"; kill $srv 2>/dev/null; return; }
    "$CLIENT" --port "$dp" --conns 20 --attack big-body --msg 1048576 --duration 4 >/dev/null 2>&1
    local c4
    c4=$(metric "http://127.0.0.1:$mp/metrics" 'status_class="4xx"')
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    awk -v c="${c4:-0}" 'BEGIN{exit !(c>0)}' && ok "http_requests{4xx}=${c4:-0} advanced" || bad "4xx did not advance"
}

# ---- S7: concurrency limiter -> in_flight <= cap ----
check_s7() {
    echo "S7 concurrency limiter vs heavy in-flight:"
    local dp=$((BASE+7)) mp=$((BASE+107))
    "$SERVER" --port "$dp" --metrics-port "$mp" --work-ms 400 --max-inflight 10 --quiet >/dev/null 2>&1 &
    local srv=$!
    wait_metrics "http://127.0.0.1:$mp/metrics" || { bad "S7 server did not start"; kill $srv 2>/dev/null; return; }
    "$CLIENT" --port "$dp" --conns 100 --msg 64 --inflight 1 --duration 4 >/dev/null 2>&1 &
    local cli=$!
    sleep 2  # sample mid-run while work is in flight
    local inflight
    inflight=$(metric "http://127.0.0.1:$mp/metrics" '^cox_requests_in_flight ')
    wait $cli 2>/dev/null
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    awk -v f="${inflight:-999999}" 'BEGIN{exit !(f<=10)}' && ok "in_flight=${inflight:-?} <= cap 10" || bad "in_flight=${inflight:-?} > cap 10"
}

echo "=== Cox hardening smoke tier (containment invariants, reduced scale) ==="
check_s5
check_s6
check_s2
check_s3
check_s4
check_s7
echo "=== smoke summary: $pass passed, $fail failed ==="
exit $(( fail > 0 ? 1 : 0 ))
