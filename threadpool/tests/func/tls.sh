#!/usr/bin/env bash
# tests/func/tls.sh — HTTPS(TLS) 기능 테스트. HTTP 포트와 HTTPS 포트를 동시에 띄워 검증한다.
#   기존 HTTP 유지, curl -k / --cacert / 검증 실패(-k 없음), openssl s_client, TLS 1.1 거부,
#   HTTPS 포트에 평문 요청·쓰레기 바이트·핸드셰이크 도중 끊기 후에도 서버 생존, 침묵 클라이언트 마감,
#   HTTPS 로 정적 대용량·CGI·리버스 프록시, 동시 연결 50개, fd 누수 없음, 시작 시 인증서 오류 거부.
# 사용: bash tests/func/tls.sh build/release/webserver
set -u
trap '' PIPE
BIN=${1:-build/release/webserver}
CGI_DIR="$(cd "$(dirname "$BIN")" && pwd)/cgi-bin"
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/lib.sh"

command -v openssl >/dev/null || { echo "openssl 명령이 필요하다"; exit 1; }

WWW="$TMP/www"
mkdir -p "$WWW"
printf 'hello-tls' >"$WWW/index.html"
head -c 5000000 /dev/urandom >"$WWW/big.bin"

bash "$HERE/../../tools/gen-cert.sh" "$TMP/certs" >/dev/null
bash "$HERE/../../tools/gen-cert.sh" "$TMP/certs2" >/dev/null # 짝이 안 맞는 키 테스트용
CRT="$TMP/certs/server.crt"
KEY="$TMP/certs/server.key"

# 리버스 프록시 백엔드 (평문 HTTP)
mkdir -p "$TMP/bwww/api"
printf 'from-backend' >"$TMP/bwww/api/index.html" # 프록시는 경로를 그대로 넘긴다
spawn_server 0 "$TMP/backend.log" "$BIN" -r "$TMP/bwww" -n B1
BPORT=$SPAWN_PORT
cat >"$TMP/tls.conf" <<EOF
route /api 127.0.0.1:$BPORT
EOF

HPORT=$((20000 + (RANDOM % 20000)))
start_server "$BIN" -f "$TMP/tls.conf" -r "$WWW" -g "$CGI_DIR" -C "$CRT" -K "$KEY" -s "$HPORT" \
    -w 8 -q 64 -t 3000 -T 2000
echo "[tls.sh] http=$PORT https=$HPORT"
surl() { echo "https://127.0.0.1:$HPORT$1"; }
fd_count() { lsof -p "$SERVER_PID" 2>/dev/null | tail -n +2 | wc -l | tr -d ' '; }
alive() { check "$1 → 서버 생존 (HTTPS 200)" 200 "$(curl -sk -o /dev/null -w '%{http_code}' --max-time 5 "$(surl /)")"; }

sleep 0.3
base=$(fd_count)

# ---- 1. 기존 HTTP 유지 ----
check "HTTP 8080 계열 포트: 기존과 같이 200" "hello-tls" "$(curl -s --max-time 5 "$(url /)")"
check "HTTP 404 유지" 404 "$(status_of /nope)"

# ---- 2. HTTPS 기본 ----
check "curl -k https → 200 본문" "hello-tls" "$(curl -sk --max-time 5 "$(surl /)")"
check "HTTPS HEAD → 200" 200 "$(curl -sk -I -o /dev/null -w '%{http_code}' --max-time 5 "$(surl /)")"
check "HTTPS 404" 404 "$(curl -sk -o /dev/null -w '%{http_code}' --max-time 5 "$(surl /nope)")"
check "TLS 1.2 이상으로 협상" yes \
    "$(curl -sk -v --max-time 5 "$(surl /)" 2>&1 | grep -qE 'TLSv1\.[23]' && echo yes || echo no)"
check "keep-alive: TLS 연결 하나로 요청 두 개 (두 번째는 새 연결 0개)" "1 0" \
    "$(curl -sk -o /dev/null -o /dev/null -w '%{num_connects} ' --max-time 5 "$(surl /)" "$(surl /index.html)" | sed 's/ $//')"

# ---- 3. 인증서 검증 ----
curl -s -o /dev/null --max-time 5 "$(surl /)"
check "-k 없이 → 자체 서명이라 검증 실패 (curl exit 60)" 60 "$?"
check "--cacert 로 이 인증서를 신뢰 + 이름 localhost → 검증 성공" "hello-tls" \
    "$(curl -s --cacert "$CRT" --max-time 5 "https://localhost:$HPORT/")"
alive "검증 실패 뒤"

# ---- 4. openssl s_client ----
first=$(printf 'GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n' |
    openssl s_client -connect "127.0.0.1:$HPORT" -quiet 2>/dev/null | head -1 | tr -d '\r')
check "openssl s_client → HTTP/1.1 200" "HTTP/1.1 200 OK" "$first"
first=$(printf 'GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n' |
    openssl s_client -connect "127.0.0.1:$HPORT" -tls1_2 -quiet 2>/dev/null | head -1 | tr -d '\r')
check "openssl s_client -tls1_2 → 200" "HTTP/1.1 200 OK" "$first"
out=$(printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\n' |
    openssl s_client -connect "127.0.0.1:$HPORT" -tls1_1 -quiet 2>&1 | head -5)
case "$out" in *"HTTP/1.1"*) r=accepted ;; *) r=rejected ;; esac
check "TLS 1.1 → 거부" rejected "$r"
alive "TLS 1.1 거부 뒤"

# ---- 5. 잘못된 접속에도 서버는 계속 동작 ----
resp=$(curl -s -i --max-time 5 "http://127.0.0.1:$HPORT/")
check_contains "HTTPS 포트에 평문 HTTP → 평문 400" "HTTP/1.1 400 Bad Request" "$resp"
check_contains "  ... 원인 안내 헤더" "plain HTTP request sent to HTTPS port" "$resp"
alive "평문 요청 뒤"
for i in $(seq 1 10); do # TLS 가 아닌 쓰레기 바이트
    exec 5<>"/dev/tcp/127.0.0.1/$HPORT"
    printf '\x16\x03\x01\x00\x05garbage-garbage' >&5 2>/dev/null
    exec 5>&-
done
for i in $(seq 1 20); do # 핸드셰이크 도중 끊기: ClientHello 앞부분만 보내고 닫음
    exec 5<>"/dev/tcp/127.0.0.1/$HPORT"
    printf '\x16\x03\x01\x02\x00\x01\x00\x01' >&5 2>/dev/null
    exec 5>&-
done
for i in $(seq 1 20); do # 연결만 하고 바로 닫음
    exec 5<>"/dev/tcp/127.0.0.1/$HPORT"
    exec 5>&-
done
alive "쓰레기 x10, 핸드셰이크 도중 끊기 x20, 빈 연결 x20"
# s_client 핸드셰이크 직후 아무 요청 없이 끊기
for i in $(seq 1 5); do openssl s_client -connect "127.0.0.1:$HPORT" </dev/null >/dev/null 2>&1; done
alive "핸드셰이크 후 요청 없이 끊기 x5"

# 침묵 클라이언트(ClientHello 를 안 보냄) → 핸드셰이크 마감(-T 2초)에 서버가 끊는다
el=$(python3 - "$HPORT" <<'PY'
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.settimeout(10)
t = time.time()
try:
    s.recv(1)
except Exception:
    pass
print(int(time.time() - t))
PY
)
if [ "$el" -le 4 ]; then check "침묵 클라이언트 → 핸드셰이크 마감 근처에서 끊김 (<=4s)" yes yes
else check "침묵 클라이언트 → 핸드셰이크 마감 근처에서 끊김 (<=4s)" yes "no(${el}s)"; fi

# ---- 6. HTTPS 로 기존 기능 (정적 대용량, CGI, 리버스 프록시) ----
curl -sk --max-time 20 -o "$TMP/big.out" "$(surl /big.bin)"
if cmp -s "$WWW/big.bin" "$TMP/big.out"; then check "HTTPS 정적 5MB 무결성 (부분 쓰기)" same same
else check "HTTPS 정적 5MB 무결성 (부분 쓰기)" same diff; fi
check "HTTPS CGI adder 3+4" 7 "$(curl -sk --max-time 5 "$(surl '/cgi-bin/adder?a=3&b=4')")"
head -c 200000 /dev/urandom >"$TMP/body.bin"
curl -sk -X POST --data-binary @"$TMP/body.bin" -o "$TMP/echo.out" --max-time 10 "$(surl /cgi-bin/echo)"
if cmp -s "$TMP/body.bin" "$TMP/echo.out"; then check "HTTPS CGI echo 200KB 왕복 (파이프 → SSL_write)" same same
else check "HTTPS CGI echo 200KB 왕복 (파이프 → SSL_write)" same diff; fi
check "HTTPS → 리버스 프록시 → 평문 백엔드" "from-backend" "$(curl -sk --max-time 5 "$(surl /api/)")"
check "  ... X-Backend 헤더" "B1" \
    "$(curl -sk -D - -o /dev/null --max-time 5 "$(surl /api/)" | awk -F': ' 'tolower($1)=="x-backend"{print $2}' | tr -d '\r')"

# ---- 7. 동시 연결 50개: 연결마다 SSL 객체가 독립 ----
pids=""
for i in $(seq 1 50); do
    curl -sk -o "$TMP/par.$i" --max-time 10 "$(surl "/cgi-bin/adder?a=$i&b=1000")" &
    pids="$pids $!"
done
for p in $pids; do wait "$p"; done
okc=0
for i in $(seq 1 50); do [ "$(cat "$TMP/par.$i" 2>/dev/null)" = "$((i + 1000))" ] && okc=$((okc + 1)); done
check "동시 HTTPS 50개 모두 자기 응답 (섞이지 않음)" 50 "$okc"
pids=""
for i in $(seq 1 25); do
    curl -s -o /dev/null --max-time 10 "$(url /)" &
    pids="$pids $!"
    curl -sk -o /dev/null --max-time 10 "$(surl /)" &
    pids="$pids $!"
done
for p in $pids; do wait "$p"; done
alive "HTTP·HTTPS 섞어서 동시 50개"

# ---- 8. 자원 회수 ----
sleep 0.6
eventually "서버 fd 개수가 처음과 같음 (소켓·SSL 누수 없음)" "$base" fd_count
log=$(cat "$SERVER_LOG")
check_contains "로그: 핸드셰이크 실패 사유 (SSL_get_error)" "핸드셰이크 실패 (SSL_ERROR_SSL)" "$log"
check_contains "로그: OpenSSL 오류 큐 (ERR_print_errors_cb → 로그)" "  openssl: " "$log"
check_contains "로그: 평문 요청 감지" "HTTPS 포트에 평문 HTTP 요청" "$log"
check_contains "로그: HTTPS 리슨" "listening on https://" "$log"

stop_server
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"

# ---- 9. 시작 시 인증서 오류 → 시작 거부 ----
startup() { # startup <옵션...> → "rc|로그"
    local out rc
    out=$("$BIN" -H 127.0.0.1 -p "$((20000 + (RANDOM % 20000)))" -r "$WWW" "$@" 2>&1)
    rc=$?
    printf '%s|%s' "$rc" "$out"
}
r=$(startup -C "$CRT" -K "$TMP/certs2/server.key" -s "$HPORT")
check "인증서·개인키 짝 불일치 → 종료 코드 1" 1 "${r%%|*}"
check_contains "  ... 원인 로그" "짝이 아님" "$r"
r=$(startup -C "$TMP/none.crt" -K "$KEY" -s "$HPORT")
check "인증서 파일 없음 → 종료 코드 1" 1 "${r%%|*}"
r=$(startup -C "$CRT" -s "$HPORT")
check "개인키 없이 -C 만 → 종료 코드 1" 1 "${r%%|*}"
check_contains "  ... 원인 로그" "둘 다 필요" "$r"

finish
