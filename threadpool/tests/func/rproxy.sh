#!/usr/bin/env bash
# tests/func/rproxy.sh — 백엔드 2개(같은 바이너리 --name A/B) + 리버스 프록시로 검증한다.
#   라운드로빈(ABABAB), Host 규칙, 요청 재작성(XFF/Via/Host), POST 중계, keep-alive,
#   504(백엔드 CGI sleep + 프록시 읽기 타임아웃), 백엔드 kill → 나머지로, down 복귀, 전부 kill → 502
# 사용: bash tests/func/rproxy.sh build/release/webserver
set -u
BIN=${1:-build/release/webserver}
CGI_DIR="$(cd "$(dirname "$BIN")" && pwd)/cgi-bin"
source "$(dirname "$0")/lib.sh"

for n in A B; do
    mkdir -p "$TMP/www$n"
    printf '%s' "$n" >"$TMP/www$n/index.html"
done
mkdir -p "$TMP/wwwP"

# ---- 백엔드 A, B: 같은 바이너리를 --name 으로 구분 ----
spawn_server 0 "$TMP/a.log" "$BIN" --name A -r "$TMP/wwwA" -g "$CGI_DIR" -c 10000
A_PID=$SPAWN_PID A_PORT=$SPAWN_PORT
spawn_server 0 "$TMP/b.log" "$BIN" -n B -r "$TMP/wwwB" -g "$CGI_DIR" -c 10000
B_PID=$SPAWN_PID B_PORT=$SPAWN_PORT

cat >"$TMP/proxy.conf" <<EOF
# 리버스 프록시 설정 (rproxy.sh 가 생성)
root $TMP/wwwP
upstream_connect_timeout_ms 500
upstream_read_timeout_ms    1000
upstream_down_secs          2
route /            127.0.0.1:$A_PORT 127.0.0.1:$B_PORT
route host=b.only / 127.0.0.1:$B_PORT
EOF
start_server "$BIN" -f "$TMP/proxy.conf" -w 8 -t 3000
echo "[rproxy.sh] proxy=$PORT A=$A_PORT B=$B_PORT"

backend_of() { # backend_of <경로> [curl 옵션...] → X-Backend 값 (없으면 -)
    local p=$1 v
    shift
    v=$(curl -s -D - -o /dev/null --max-time 5 "$@" "$(url "$p")" | tr -d '\r' |
        awk -F': ' 'tolower($1) == "x-backend" { print $2 }')
    echo "${v:--}"
}
seq_of() { # seq_of <횟수> [curl 옵션...] → "ABAB..."
    local n=$1 i s=""
    shift
    for i in $(seq 1 "$n"); do s="$s$(backend_of / "$@")"; done
    echo "$s"
}

# ---- 라운드로빈 ----
s=$(seq_of 6)
echo "    6회 응답 순서: $s"
case "$s" in ABABAB | BABABA) check "curl 6번 → A/B 번갈아" alt alt ;;
*) check "curl 6번 → A/B 번갈아" alt "$s" ;; esac
body=$(curl -s "$(url /)")
case "$body" in A | B) check "본문은 백엔드의 index.html" ok ok ;;
*) check "본문은 백엔드의 index.html" ok "$body" ;; esac

# ---- Host 규칙: host=b.only 는 B 로만 ----
check "Host: b.only → 항상 B" BBBB "$(seq_of 4 -H 'Host: b.only')"
check "Host: B.ONLY:1234 (대소문자·포트 무시)" BBBB "$(seq_of 4 -H 'Host: B.ONLY:1234')"

# ---- 요청 재작성: 백엔드가 받은 헤더를 env CGI 로 확인 ----
envout=$(curl -s -H 'Proxy: http://evil.example' -H 'X-Forwarded-For: 203.0.113.7' \
    -H 'Connection: keep-alive, X-Drop' -H 'X-Drop: secret' "$(url /cgi-bin/env)")
check_contains "XFF: 기존 값 + 클라이언트 IP" "HTTP_X_FORWARDED_FOR=203.0.113.7, 127.0.0.1" "$envout"
check_contains "X-Forwarded-Proto" "HTTP_X_FORWARDED_PROTO=http" "$envout"
check_contains "Via" "HTTP_VIA=1.1 cweb" "$envout"
check_contains "업스트림엔 Connection: close" "HTTP_CONNECTION=close" "$envout"
check_contains "Host 보존" "HTTP_HOST=127.0.0.1:$PORT" "$envout"
check_contains "httpoxy: Proxy 헤더는 HTTP_PROXY 로 안 감" "HTTP_PROXY=(unset)" "$envout"
case "$envout" in *secret*) check "Connection 에 나열된 헤더(X-Drop) 제거" removed leaked ;;
*) check "Connection 에 나열된 헤더(X-Drop) 제거" removed removed ;; esac

# ---- POST 본문 중계 ----
head -c 100000 /dev/urandom >"$TMP/body.bin"
curl -s -X POST --data-binary @"$TMP/body.bin" -o "$TMP/echo.out" "$(url /cgi-bin/echo)"
if cmp -s "$TMP/body.bin" "$TMP/echo.out"; then check "POST 100KB 왕복 (프록시→CGI)" same same
else check "POST 100KB 왕복 (프록시→CGI)" same different; fi

# ---- 클라이언트 keep-alive 유지 (백엔드 응답에 Content-Length 가 있으므로) ----
check "프록시 경유 3요청에 연결 1개 (100)" 100 "$(curl -s -o /dev/null -o /dev/null -o /dev/null \
    -w '%{num_connects}' "$(url /)" "$(url /)" "$(url /)")"
check_contains "응답 Connection 은 프록시가 결정" "Connection: keep-alive" "$(curl -s -i "$(url /)")"

# ---- 504: 백엔드 CGI 가 10초 자는 동안 프록시 읽기 타임아웃 1초 ----
t0=$(date +%s)
check "느린 백엔드 → 504" 504 "$(status_of '/cgi-bin/sleep?s=10' --max-time 8)"
t1=$(date +%s)
if [ $((t1 - t0)) -le 3 ]; then check "504 는 읽기 타임아웃 근처에서 (<=3s)" yes yes
else check "504 는 읽기 타임아웃 근처에서 (<=3s)" yes "no($((t1 - t0))s)"; fi
check "읽기 타임아웃은 down 표시 안 함 → 계속 A/B 번갈아" 2 \
    "$(seq_of 4 | fold -w1 | sort -u | wc -l | tr -d ' ')"

# ---- 백엔드 A 를 죽이면 B 로 (첫 요청도 재시도로 성공) ----
kill -KILL "$A_PID"; wait "$A_PID" 2>/dev/null
s=$(seq_of 6)
check "A kill 후 6회 모두 B" BBBBBB "$s"
codes=""
for i in 1 2 3 4; do codes="$codes$(status_of /)"; done
check "A kill 후에도 전부 200" 200200200200 "$codes"
check_contains "프록시 로그: A down 표시" "127.0.0.1:$A_PORT 연결 실패" "$(cat "$SERVER_LOG")"

# ---- down 기간(2초)이 지나면 A 복귀 ----
spawn_server "$A_PORT" "$TMP/a.log" "$BIN" --name A -r "$TMP/wwwA" -g "$CGI_DIR"
A_PID=$SPAWN_PID
sleep 2.2
s=$(seq_of 6)
case "$s" in *A*) check "down 기간 후 A 복귀" back back ;; *) check "down 기간 후 A 복귀" back "$s" ;; esac

# ---- 전부 죽이면 502 ----
kill -KILL "$A_PID" "$B_PID"; wait "$A_PID" "$B_PID" 2>/dev/null
check "백엔드 전부 kill → 502" 502 "$(status_of /)"
check "down 표시 중에도 즉시 502" 502 "$(status_of /)"
check "Host 규칙 풀도 502" 502 "$(status_of / -H 'Host: b.only')"

stop_server
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"
finish
