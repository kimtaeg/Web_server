#!/usr/bin/env bash
# tests/func/cgi.sh — 실제 서버 + 예제 CGI(adder/echo/sleep/env)로 CGI 를 검증한다.
# 사용: bash tests/func/cgi.sh build/release/webserver
#   CGI 디렉터리는 바이너리 옆의 cgi-bin/ (make 가 cgi-bin/*.c 를 거기에 빌드한다)
set -u
BIN=${1:-build/release/webserver}
CGI_DIR="$(cd "$(dirname "$BIN")" && pwd)/cgi-bin"
source "$(dirname "$0")/lib.sh"

WWW="$TMP/www"
mkdir -p "$WWW"
printf 'static-ok' >"$WWW/index.html"

start_server "$BIN" -r "$WWW" -g "$CGI_DIR" -c 1500 -w 8 -q 32 -t 3000
echo "[cgi.sh] port=$PORT cgi=$CGI_DIR"

zombies() { # 서버의 자식 중 좀비(Z) 수
    ps -A -o ppid=,stat= | awk -v p="$SERVER_PID" '$1 == p && $2 ~ /^Z/' | wc -l | tr -d ' '
}
children() { # 서버의 살아 있는 자식 수
    ps -A -o ppid= | awk -v p="$SERVER_PID" '$1 == p' | wc -l | tr -d ' '
}

# ---- adder: 쿼리의 두 수의 합 ----
check "adder 3+4" 7 "$(curl -s "$(url '/cgi-bin/adder?a=3&b=4')")"
check "adder 음수" -5 "$(curl -s "$(url '/cgi-bin/adder?a=10&b=-15')")"
check "adder 순서 무관" 9 "$(curl -s "$(url '/cgi-bin/adder?b=4&a=5')")"
check "adder 200" 200 "$(status_of '/cgi-bin/adder?a=1&b=2')"
check "adder Content-Type (CGI 헤더 전달)" "text/plain; charset=utf-8" \
    "$(curl -s -o /dev/null -w '%{content_type}' "$(url '/cgi-bin/adder?a=1&b=2')")"
check "adder 잘못된 입력 → CGI Status: 400" 400 "$(status_of '/cgi-bin/adder?a=x&b=2')"
check "adder 인자 없음 → 400" 400 "$(status_of '/cgi-bin/adder')"
check "adder 오버플로 → 400" 400 "$(status_of '/cgi-bin/adder?a=9223372036854775807&b=1')"

# ---- echo: POST 본문을 그대로 ----
check "echo 텍스트" "hello cgi" "$(curl -s -X POST --data-binary 'hello cgi' "$(url /cgi-bin/echo)")"
head -c 200000 /dev/urandom >"$TMP/body.bin"
curl -s -X POST -H 'Content-Type: application/x-test' --data-binary @"$TMP/body.bin" \
    -o "$TMP/echo.out" -D "$TMP/echo.hdr" "$(url /cgi-bin/echo)"
if cmp -s "$TMP/body.bin" "$TMP/echo.out"; then check "echo 200KB 바이너리 왕복" same same
else check "echo 200KB 바이너리 왕복" same different; fi
check_contains "echo 가 CONTENT_TYPE 반영" "Content-Type: application/x-test" "$(cat "$TMP/echo.hdr")"
check_contains "Content-Length 는 서버가 계산" "Content-Length: 200000" "$(cat "$TMP/echo.hdr")"
check "빈 본문 POST" "" "$(curl -s -X POST -H 'Content-Length: 0' "$(url /cgi-bin/echo)")"

# ---- env: CGI 환경 변수 ----
envout=$(curl -s -X POST -H 'Content-Type: text/plain' --data-binary 'abc' "$(url '/cgi-bin/env?k=v&z=1')")
check_contains "REQUEST_METHOD" "REQUEST_METHOD=POST" "$envout"
check_contains "QUERY_STRING" "QUERY_STRING=k=v&z=1" "$envout"
check_contains "CONTENT_LENGTH" "CONTENT_LENGTH=3" "$envout"
check_contains "CONTENT_TYPE" "CONTENT_TYPE=text/plain" "$envout"
check_contains "SCRIPT_NAME" "SCRIPT_NAME=/cgi-bin/env" "$envout"
check_contains "SERVER_PROTOCOL" "SERVER_PROTOCOL=HTTP/1.1" "$envout"
check_contains "REMOTE_ADDR" "REMOTE_ADDR=127.0.0.1" "$envout"
check_contains "GATEWAY_INTERFACE" "GATEWAY_INTERFACE=CGI/1.1" "$envout"

# ---- 타임아웃: sleep(10초) 을 -c 1500 으로 → 504, 프로세스 회수 ----
t0=$(date +%s)
check "sleep 타임아웃 → 504" 504 "$(status_of '/cgi-bin/sleep?s=10' --max-time 8)"
t1=$(date +%s)
if [ $((t1 - t0)) -le 4 ]; then check "504 가 타임아웃 근처에서 응답 (<=4s)" yes yes
else check "504 가 타임아웃 근처에서 응답 (<=4s)" yes "no($((t1 - t0))s)"; fi
sleep 0.2
check "타임아웃 후 sleep 프로세스 남지 않음" 0 "$(pgrep -f "$CGI_DIR/sleep" | wc -l | tr -d ' ')"
check_contains "서버 로그에 타임아웃 기록" "타임아웃" "$(cat "$SERVER_LOG")"

# ---- 오류 경로 ----
check "없는 스크립트 → 404" 404 "$(status_of /cgi-bin/nope)"
check "/cgi-bin/ → 404" 404 "$(status_of /cgi-bin/)"
check "/cgi-bin/../ 탈출 → 404" 404 "$(status_of /cgi-bin/../index.html --path-as-is)"
check "/cgi-bin/%2e%2e → 404" 404 "$(status_of /cgi-bin/%2e%2e --path-as-is)"
check "하위 경로 → 404" 404 "$(status_of /cgi-bin/adder/x)"
check "PUT → 405" 405 "$(status_of /cgi-bin/adder -X PUT -d x)"
check_contains "405 Allow" "Allow: GET, HEAD, POST" "$(curl -s -i -X PUT -d x "$(url /cgi-bin/adder)")"
check "HEAD → 200" 200 "$(status_of '/cgi-bin/adder?a=1&b=1' -I)"
check "HEAD 본문 없음" 0 "$(curl -s -I -o /dev/null -w '%{size_download}' "$(url '/cgi-bin/adder?a=1&b=1')")"

# ---- 라우팅: 정적 파일은 그대로, CGI 디렉터리는 정적으로 노출되지 않음 ----
check "정적 파일 여전히 동작" "static-ok" "$(curl -s "$(url /)")"
check "/cgi-binx 는 static 으로 (404)" 404 "$(status_of /cgi-binx)"

# ---- keep-alive: CGI 응답 뒤에도 연결 재사용 ----
check "CGI 2회에 연결 1개 (10)" 10 "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects}' \
    "$(url '/cgi-bin/adder?a=1&b=1')" "$(url '/cgi-bin/adder?a=2&b=2')")"

# ---- 동시 실행 ----
pids=""
for i in $(seq 1 16); do
    curl -s -o "$TMP/par.$i" "$(url "/cgi-bin/adder?a=$i&b=$i")" &
    pids="$pids $!"
done
for p in $pids; do wait "$p"; done
ok=0
for i in $(seq 1 16); do [ "$(cat "$TMP/par.$i")" = "$((i * 2))" ] && ok=$((ok + 1)); done
check "동시 16개 모두 정답" 16 "$ok"

# ---- 좀비 없음 ----
sleep 0.3
check "서버의 좀비 자식 0개" 0 "$(zombies)"
check "서버의 남은 자식 프로세스 0개" 0 "$(children)"

stop_server
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"
finish
