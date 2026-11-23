#!/bin/bash
# Functional tests with real HTTP clients (curl + raw sockets). Usage: tests/functional.sh [build-dir]
. "$(dirname "$0")/lib.sh"
trap 'stop_all; rm -rf www/__test' EXIT

start_backend $B1 api-1
start_backend $B2 api-2
start_backend $B3 app
wait_port $B1 && wait_port $B2 && wait_port $B3
start_server || exit 1

mkdir -p www/__test
echo secret > www/__test/noperm.txt && chmod 000 www/__test/noperm.txt
ln -sf /etc/hosts www/__test/escape
head -c 3000000 /dev/zero | tr '\0' 'x' > www/__test/big.bin

echo "== static files"
check "GET / -> 200" 200 "$(code $BASE/)"
check "index is text/html" "text/html; charset=utf-8" "$(header Content-Type $BASE/)"
check "css mime" "text/css; charset=utf-8" "$(header Content-Type $BASE/css/style.css)"
check "js mime" "text/javascript; charset=utf-8" "$(header Content-Type $BASE/js/app.js)"
check "png mime" "image/png" "$(header Content-Type $BASE/img/logo.png)"
check "jpg mime" "image/jpeg" "$(header Content-Type $BASE/img/photo.jpg)"
check "unknown extension -> octet-stream" "application/octet-stream" "$(header Content-Type $BASE/docs/notes.unknownext)"
check "nosniff on typed responses" "nosniff" "$(header X-Content-Type-Options $BASE/)"
check "png bytes intact" "$(shasum < www/img/logo.png)" "$(curl -s $BASE/img/logo.png | shasum)"
check "3 MB file via sendfile intact" "$(shasum < www/__test/big.bin)" "$(curl -s $BASE/__test/big.bin | shasum)"
check "HEAD has Content-Length" "$(wc -c < www/index.html | tr -d ' ')" "$(curl -s -I $BASE/ | tr -d '\r' | awk '/^Content-Length/{print $2}')"
check "HEAD has no body" 0 "$(curl -s -I $BASE/ -o /dev/null -w '%{size_download}')"
check "missing file -> 404" 404 "$(code $BASE/nope.html)"
check "dotfile -> 403" 403 "$(code $BASE/.env)"
check "unreadable file -> 403" 403 "$(code $BASE/__test/noperm.txt)"
check "symlink out of root -> 403" 403 "$(code $BASE/__test/escape)"
check "../ traversal -> 403" 403 "$(code --path-as-is $BASE/../../etc/passwd)"
check "%2e%2e traversal -> 403" 403 "$(code --path-as-is $BASE/%2e%2e/%2e%2e/etc/passwd)"
check "dir without slash -> 301" 301 "$(code $BASE/docs)"
check "301 Location" "/docs/" "$(header Location $BASE/docs)"
check "dir index -> 200" 200 "$(code $BASE/docs/)"
check "POST to static -> 405" 405 "$(code -X POST -d x=1 $BASE/index.html)"
check "405 has Allow" "GET, HEAD" "$(header Allow -X DELETE $BASE/index.html)"
etag=$(header ETag $BASE/css/style.css)
check "If-None-Match -> 304" 304 "$(code -H "If-None-Match: $etag" $BASE/css/style.css)"
lm=$(header Last-Modified $BASE/css/style.css)
check "If-Modified-Since -> 304" 304 "$(code -H "If-Modified-Since: $lm" $BASE/css/style.css)"
check "percent-encoded path" 200 "$(code $BASE/%69ndex.html)"
check "OPTIONS * -> 200" 200 "$(code -X OPTIONS --request-target '*' $BASE)"
check "TRACE -> 405" 405 "$(code -X TRACE $BASE/)"

echo "== CGI"
out=$(curl -s "$BASE/cgi-bin/hello.sh/extra?name=world")
contains "CGI GET runs script" "Hello from CGI" "$out"
contains "CGI QUERY_STRING" "QUERY_STRING=name=world" "$out"
contains "CGI PATH_INFO" "PATH_INFO=/extra" "$out"
check "CGI custom header passes" "hello" "$(header X-CGI $BASE/cgi-bin/hello.sh)"
contains "CGI POST body (escaped)" "a=1&amp;b=&lt;2&gt;" "$(curl -s -d 'a=1&b=<2>' $BASE/cgi-bin/form.sh)"
contains "CGI chunked POST body" "chunky" "$(curl -s -H 'Transfer-Encoding: chunked' -d 'chunky' $BASE/cgi-bin/form.sh)"
check "CGI Status: 201" 201 "$(code $BASE/cgi-bin/created.sh)"
check "CGI Location" "/items/42" "$(header Location $BASE/cgi-bin/created.sh)"
check "CGI bad output -> 502" 502 "$(code $BASE/cgi-bin/broken.sh)"
check "CGI timeout -> 504" 504 "$(code $BASE/cgi-bin/slow.sh)"
sleep 0.2
check "timeout kills the script's own children too" 0 "$(pgrep -f '^sleep 37' | wc -l | tr -d ' ')"
check "CGI not executable -> 403" 403 "$(code $BASE/cgi-bin/not_executable.sh)"
check "CGI missing -> 404" 404 "$(code $BASE/cgi-bin/missing.sh)"
check "CGI PUT -> 405" 405 "$(code -X PUT $BASE/cgi-bin/hello.sh)"
check "CGI child does not inherit the blocked signal mask" "SIGTERM delivered" "$(curl -s $BASE/cgi-bin/signals.sh)"

echo "== reverse proxy"
seq_out=""
for i in 1 2 3 4; do
    seq_out="$seq_out $(curl -s $BASE/api/rr | sed 's/.*"backend": "\([^"]*\)".*/\1/')"
done
check "round-robin alternates" " api-1 api-2 api-1 api-2" "$seq_out"
contains "prefix stripped, query kept" '"target": "/users?id=7"' "$(curl -s "$BASE/api/users?id=7")"
contains "X-Forwarded-For added" '"X-Forwarded-For": "127.0.0.1"' "$(curl -s $BASE/api/x)"
contains "Via added" '"Via": "1.1 cwebserv"' "$(curl -s $BASE/api/x)"
contains "POST body forwarded" '"body_bytes": 11' "$(curl -s -d 'hello=world' $BASE/api/form)"
contains "chunked request body re-framed" '"body_bytes": 5' "$(curl -s -H 'Transfer-Encoding: chunked' -d 'abcde' $BASE/api/form)"
contains "host-based route" '"backend": "app"' "$(curl -s -H 'Host: app.localhost' $BASE/anything)"
check "chunked upstream response relayed" "hello world!" "$(curl -s $BASE/api/chunked)"
contains "close-delimited upstream response" '"backend"' "$(curl -s $BASE/api/close)"
check "garbage from upstream -> 502" 502 "$(code $BASE/api/garbage)"
check "dead upstream -> 502" 502 "$(code $BASE/dead/x)"
start=$(date +%s)
check "slow upstream -> 504" 504 "$(code "$BASE/slowapi/slow?ms=3000")"
check "504 within ~proxy_timeout" yes "$([ $(( $(date +%s) - start )) -le 2 ] && echo yes || echo no)"
contains "/apix is not routed to /api" "404" "$(code $BASE/apix)"

kill "$BACKEND_19002"; wait "$BACKEND_19002" 2>/dev/null
ok=0
for i in 1 2 3 4 5 6; do [ "$(code $BASE/api/failover)" = 200 ] && ok=$((ok + 1)); done
check "failover: all 6 succeed with one backend down" 6 "$ok"
contains "status page marks backend down" '"addr": "127.0.0.1:19002", "healthy": false' "$(curl -s $BASE/__status)"
start_backend $B2 api-2
wait_port $B2

echo "== forward proxy + cache"
miss=$(curl -s -D "$LOG_DIR/miss.h" -x $BASE http://127.0.0.1:$B1/cacheable)
contains "first GET is a MISS" "X-Cache: MISS" "$(cat "$LOG_DIR/miss.h")"
hit=$(curl -s -D "$LOG_DIR/hit.h" -x $BASE http://127.0.0.1:$B1/cacheable)
contains "second GET is a HIT" "X-Cache: HIT" "$(cat "$LOG_DIR/hit.h")"
check "HIT body is the stored body (backend not called)" "$miss" "$hit"
contains "Age header on HIT" "Age: " "$(cat "$LOG_DIR/hit.h")"
check "no-cache request bypasses cache" "BYPASS" "$(header X-Cache -H 'Cache-Control: no-cache' -x $BASE http://127.0.0.1:$B1/cacheable)"
check "forward chunked" "hello world!" "$(curl -s -x $BASE http://127.0.0.1:$B2/chunked)"
check "Via already contains us -> 508 (loop guard)" 508 "$(code -H "Via: 1.1 other, 1.1 cwebserv" -x $BASE http://127.0.0.1:$B1/)"
contains "proxying to ourselves terminates (origin-form 2nd hop)" "<!doctype html>" "$(curl -s -x $BASE $BASE/)"
check "unreachable origin -> 502" 502 "$(code -x $BASE http://127.0.0.1:$DEAD/)"
contains "CONNECT tunnel" '"backend": "api-1"' "$(curl -s -p -x $BASE http://127.0.0.1:$B1/tunnel)"
contains "status page has cache stats" '"hits": ' "$(curl -s $BASE/__status)"

echo "== connection handling"
v=$(curl -sv $BASE/ $BASE/css/style.css -o /dev/null -o /dev/null 2>&1)
contains "keep-alive reuses the connection" "Re-using existing connection" "$v"
check "HTTP/1.0 gets Connection: close" "close" "$(header Connection --http1.0 $BASE/)"
check "HTTP/1.0 + keep-alive honoured" "keep-alive" "$(header Connection --http1.0 -H 'Connection: keep-alive' $BASE/)"
check "body over max_body_size -> 413" 413 "$(head -c 2000000 /dev/zero | curl -s -o /dev/null -w '%{http_code}' --data-binary @- $BASE/cgi-bin/form.sh)"
codes=$(seq 1 100 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\n' "$BASE/api/c{}" | sort | uniq -c | tr -s ' ')
check "100 concurrent proxied requests all 200" " 100 200" "$codes"
codes=$(seq 1 200 | xargs -P 100 -I{} curl -s -o /dev/null -w '%{http_code}\n' "$BASE/index.html" | sort | uniq -c | tr -s ' ')
check "200 concurrent static requests all 200" " 200 200" "$codes"

echo "== raw-socket edge cases"
python3 tests/edge_cases.py 127.0.0.1 "$WS_PORT"
edge_rc=$?

echo "== graceful shutdown"
kill -TERM "$SERVER_PID"
for _ in $(seq 1 30); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.1; done
check "SIGTERM stops the server within 3 s" gone "$(kill -0 "$SERVER_PID" 2>/dev/null && echo running || echo gone)"
wait "$SERVER_PID"; check "exit status 0" 0 "$?"
SERVER_PID=""
contains "shutdown is logged" "shutting down" "$(cat "$LOG_DIR/server.log")"

echo
echo "functional: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && [ "$edge_rc" -eq 0 ]
