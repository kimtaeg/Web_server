#!/bin/bash
# Leak check under real traffic: macOS leaks(1) --atExit, or valgrind on Linux.
# Usage: tests/memcheck.sh [build-dir]
. "$(dirname "$0")/lib.sh"
trap 'stop_all' EXIT
OUT="$LOG_DIR/memcheck.txt"

if [ "$(uname -s)" = Darwin ]; then
    export MallocStackLogging=1
    WS_WRAPPER="leaks --quiet --atExit --"
elif command -v valgrind >/dev/null; then
    WS_WRAPPER="valgrind --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --error-exitcode=9 --log-file=$OUT"
else
    echo "neither leaks(1) nor valgrind found" >&2
    exit 1
fi

start_backend $B1 api-1
start_backend $B2 api-2
start_backend $B3 app
wait_port $B1 && wait_port $B2 && wait_port $B3
start_server
unset MallocStackLogging

echo "== traffic: static, CGI, reverse/forward proxy, cache, errors"
for i in $(seq 1 30); do
    for u in / /img/photo.jpg /nope /.env /docs; do curl -s -o /dev/null $BASE$u; done
    curl -s -o /dev/null $BASE/api/x$i -d "body=$i"
    curl -s -o /dev/null -x $BASE http://127.0.0.1:$B1/cacheable
    curl -s -o /dev/null -x $BASE "http://127.0.0.1:$B2/item?$i"
done
for s in hello.sh form.sh created.sh broken.sh slow.sh missing.sh; do
    curl -s -o /dev/null -d x=1 $BASE/cgi-bin/$s
done
for u in /dead/x "/slowapi/slow?ms=3000" /api/garbage /api/chunked; do curl -s -o /dev/null "$BASE$u"; done
curl -s -o /dev/null -p -x $BASE http://127.0.0.1:$B1/tunnel
seq 1 200 | xargs -P 50 -I{} curl -s -o /dev/null $BASE/api/p{}
mkdir -p www/__test && head -c 3000000 /dev/zero > www/__test/big.bin
python3 tests/edge_cases.py 127.0.0.1 "$WS_PORT" > "$LOG_DIR/memcheck-edge.txt" 2>&1
rm -rf www/__test
python3 -c "
import socket
idle = [socket.create_connection(('127.0.0.1', $WS_PORT)) for _ in range(50)]
for s in idle[:25]: s.sendall(b'GET / HTTP/1.1\r\nHost: t\r\n\r\n')
" # connections still open (idle or mid-request) at shutdown must be freed too

echo "== SIGTERM with connections still open"
pid=$(pgrep -n -x webserv) # not -f: that would also match the leaks wrapper
kill -TERM "$pid"
wait "$SERVER_PID"
rc=$?
SERVER_PID=""
if [ "$(uname -s)" = Darwin ]; then
    cp "$LOG_DIR/server.log" "$OUT"
    summary=$(grep -E "leaks? for .* total leaked bytes" "$OUT")
    echo "$summary"
    case "$summary" in
        *" 0 leaks for 0 total leaked bytes"*) echo "memcheck: no leaks"; exit 0 ;;
        *) echo "memcheck: LEAKS FOUND (details in $OUT)"; exit 1 ;;
    esac
else
    grep -E "definitely lost|indirectly lost|ERROR SUMMARY" "$OUT"
    [ "$rc" -eq 0 ] && echo "memcheck: clean" || { echo "memcheck: errors (see $OUT)"; exit 1; }
fi
