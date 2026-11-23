#!/bin/bash
# Throughput under increasing concurrency with ApacheBench. Usage: tests/load.sh [build-dir]
# Prints a Markdown table (also written to $LOG_DIR/load.md).
. "$(dirname "$0")/lib.sh"
trap 'stop_all; rm -rf www/__bench' EXIT
command -v ab >/dev/null || { echo "ab (ApacheBench) not found" >&2; exit 1; }
mkdir -p www/__bench && head -c 3000000 /dev/zero > www/__bench/big.bin
THREADS=${THREADS:-8}
OUT="$LOG_DIR/load.md"

start_backend $B1 api-1
start_backend $B2 api-2
start_backend $B3 app
wait_port $B1 && wait_port $B2 && wait_port $B3

run_ab() { # label n c keepalive(0/1) url [extra ab args]
    label=$1 n=$2 c=$3 k=$4 url=$5
    shift 5
    flag=""
    [ "$k" = 1 ] && flag="-k"
    res=$(ab -q -r -n "$n" -c "$c" $flag "$@" "$url" 2>&1)
    rps=$(echo "$res" | awk '/Requests per second/{print $4}')
    p50=$(echo "$res" | awk '/ 50%/{print $2}')
    p99=$(echo "$res" | awk '/ 99%/{print $2}')
    # ab counts a body whose length differs from the first response as "failed"; the echo
    # backend's JSON varies in length, so only connect/receive/exception errors are real.
    failed=$(echo "$res" | awk -F'[:,)]' '/\(Connect:/{print $2 + $4 + $8}')
    [ -z "$failed" ] && failed=$(echo "$res" | awk '/Failed requests/{print $3}')
    non2xx=$(echo "$res" | awk '/Non-2xx responses/{print $3}')
    printf '| %-34s | %5s | %4s | %9s | %5s | %5s | %s |\n' "$label" "$c" \
        "$([ "$k" = 1 ] && echo yes || echo no)" "${rps:-?}" "${p50:-?}" "${p99:-?}" \
        "${failed:-?} / ${non2xx:-0}" | tee -a "$OUT"
}

header() {
    printf '\n%s\n\n' "$1" | tee -a "$OUT"
    printf '| %-34s | %5s | %4s | %9s | %5s | %5s | %s |\n' scenario conc k-a "req/s" "p50ms" "p99ms" "errors / non-2xx" | tee -a "$OUT"
    printf '|%s|------:|-----:|----------:|------:|------:|---|\n' "------------------------------------" | tee -a "$OUT"
}

: > "$OUT"
for t in 1 $THREADS; do
    start_server -q -t "$t" -s "keepalive_timeout 30" -s "request_timeout 30" -s "proxy_timeout 5000"
    curl -s -o /dev/null -x $BASE http://127.0.0.1:$B1/cacheable # warm the cache
    header "### worker threads = $t  ($(uname -s) $(uname -m), $(sysctl -n hw.ncpu 2>/dev/null || nproc) CPUs)"
    for c in 1 10 50 100 200; do
        run_ab "static /index.html (2 KB)" 20000 "$c" 1 "$BASE/index.html"
    done
    run_ab "static /index.html, new conn each" 5000 50 0 "$BASE/index.html"
    run_ab "static 3 MB file (sendfile)" 300 10 1 "$BASE/__bench/big.bin"
    run_ab "reverse proxy /api -> 2 backends" 10000 50 1 "$BASE/api/bench"
    run_ab "forward proxy, cache HIT" 10000 50 1 "http://127.0.0.1:$B1/cacheable" -X "127.0.0.1:$WS_PORT"
    run_ab "CGI fork+exec /cgi-bin/hello.sh" 1000 10 1 "$BASE/cgi-bin/hello.sh"
    kill "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null; SERVER_PID=""
done
echo
echo "results saved to $OUT"
