#!/usr/bin/env bash
# tests/func/fproxy.sh — 포워드 프록시 + 캐시를 curl -x 로 검증한다.
#   원본 서버(우리 바이너리 + 예제 CGI) ← 포워드 프록시(forward_proxy on) ← curl -x
# 사용: bash tests/func/fproxy.sh build/release/webserver
set -u
BIN=${1:-build/release/webserver}
CGI_DIR="$(cd "$(dirname "$BIN")" && pwd)/cgi-bin"
source "$(dirname "$0")/lib.sh"

mkdir -p "$TMP/origin" "$TMP/proxyroot"
printf 'origin-index' >"$TMP/origin/index.html"

# ---- 원본 서버 ----
spawn_server 0 "$TMP/origin.log" "$BIN" -r "$TMP/origin" -g "$CGI_DIR" --name ORIGIN
O_PID=$SPAWN_PID O_PORT=$SPAWN_PORT

cat >"$TMP/fproxy.conf" <<EOF
root $TMP/proxyroot
forward_proxy on
cache_max_bytes 2000000
cache_max_object_bytes 100000
cache_default_ttl_secs 60
upstream_connect_timeout_ms 500
upstream_read_timeout_ms 3000
EOF
start_server "$BIN" -f "$TMP/fproxy.conf" -w 8
echo "[fproxy.sh] proxy=$PORT origin=$O_PORT"
PX="127.0.0.1:$PORT"
O="http://localhost:$O_PORT" # localhost → ::1 이 먼저 나와도 127.0.0.1 로 넘어가야 한다

xcache() { # xcache <URL> [curl 옵션...] → X-Cache 값
    local u=$1
    shift
    curl -s -x "$PX" -D - -o /dev/null --max-time 5 "$@" "$u" | tr -d '\r' |
        awk -F': ' 'tolower($1) == "x-cache" { print $2 }'
}
body() { curl -s -x "$PX" --max-time 5 "$@"; }

# ---- 요구사항: curl -x 로 MISS → HIT ----
check "첫 요청 MISS" MISS "$(xcache "$O/")"
check "두 번째 HIT" HIT "$(xcache "$O/")"
check "HIT 본문 = 원본" "origin-index" "$(body "$O/")"
check_contains "HIT 에도 원본 헤더 유지 (X-Backend)" "X-Backend: ORIGIN" \
    "$(curl -s -x "$PX" -i "$O/")"
check "Host 대소문자/:80 정규화 → 같은 키" HIT "$(xcache "http://LOCALHOST:$O_PORT/")"

# ---- 동적 응답: 같은 토큰이면 캐시에서 온 것 ----
a=$(body "$O/cgi-bin/cachectl?id=1")
b=$(body "$O/cgi-bin/cachectl?id=1")
check "캐시 가능한 CGI 응답: 두 번째는 같은 본문 (HIT)" "$a" "$b"
c=$(body "$O/cgi-bin/cachectl?id=2")
if [ "$a" != "$c" ]; then check "쿼리가 다르면 다른 키" diff diff; else check "쿼리가 다르면 다른 키" diff same; fi

# ---- 저장하면 안 되는 응답: 항상 MISS, 본문도 매번 다름 ----
for q in "cc=no-store" "cc=private" "cc=no-cache" "cc=max-age=0" "cookie=1"; do
    x1=$(xcache "$O/cgi-bin/cachectl?$q")
    x2=$(xcache "$O/cgi-bin/cachectl?$q")
    check "$q → 저장 안 함 (MISS MISS)" "MISS MISS" "$x1 $x2"
done
x1=$(xcache "$O/cgi-bin/cachectl?size=150000")
x2=$(xcache "$O/cgi-bin/cachectl?size=150000")
check "객체 상한(100KB) 초과 → 저장 안 함" "MISS MISS" "$x1 $x2"
x1=$(xcache "$O/nope")
x2=$(xcache "$O/nope")
check "404 → 저장 안 함" "MISS MISS" "$x1 $x2"
x1=$(xcache "$O/cgi-bin/echo" -X POST -d hi)
x2=$(xcache "$O/cgi-bin/echo" -X POST -d hi)
check "POST → 저장 안 함" "MISS MISS" "$x1 $x2"
check "POST 본문 중계" "hi" "$(body "$O/cgi-bin/echo" -X POST -d hi)"
x1=$(xcache "$O/" -H 'Authorization: Basic eDp5')
check "Authorization 요청은 캐시된 / 가 있어도 건너뜀 (MISS)" MISS "$x1"

# ---- TTL: max-age=1 → 1초 뒤 만료 ----
check "max-age=1 첫 요청" MISS "$(xcache "$O/cgi-bin/cachectl?cc=max-age=1")"
check "max-age=1 즉시 재요청" HIT "$(xcache "$O/cgi-bin/cachectl?cc=max-age=1")"
sleep 1.3
check "max-age=1 1.3초 뒤 → 만료 MISS" MISS "$(xcache "$O/cgi-bin/cachectl?cc=max-age=1")"

# ---- HEAD 는 캐시된 GET 으로 HIT ----
check "HEAD → HIT" HIT "$(xcache "$O/" -I)"
check "HEAD HIT 본문 없음" 0 "$(curl -s -x "$PX" -I -o /dev/null -w '%{size_download}' "$O/")"

# ---- keep-alive: 클라이언트↔프록시 연결 재사용 (HIT 와 MISS 섞어서) ----
check "프록시 연결 재사용 (100)" 100 "$(curl -s -x "$PX" -o /dev/null -o /dev/null -o /dev/null \
    -w '%{num_connects}' "$O/" "$O/cgi-bin/cachectl?cc=no-store" "$O/")"

# ---- 지원하지 않는 것 / 실패 ----
check "https (curl 은 CONNECT 사용) → 501" 501 \
    "$(curl -s -x "$PX" -o /dev/null -w '%{http_connect}' --max-time 5 https://example.com/)"
exec 5<>"/dev/tcp/127.0.0.1/$PORT"
printf 'GET https://example.com/ HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n' >&5
line=$(head -1 <&5 | tr -d '\r')
exec 5>&-
check "https:// absolute-form → 501" "HTTP/1.1 501 Not Implemented" "$line"
check "없는 호스트 → 502" 502 "$(curl -s -x "$PX" -o /dev/null -w '%{http_code}' http://no-such-host.invalid/)"
check "닫힌 포트 → 502" 502 "$(curl -s -x "$PX" -o /dev/null -w '%{http_code}' http://127.0.0.1:1/)"

# ---- 루프 감지: 자기 Via 표식이 붙은 요청 → 508 ----
via=$(sed -n 's/.*Via 표식: //p' "$SERVER_LOG" | head -1)
check "508 Loop Detected" 508 "$(curl -s -x "$PX" -o /dev/null -w '%{http_code}' -H "Via: $via" "$O/")"
check "다른 Via 는 정상 통과" 200 "$(curl -s -x "$PX" -o /dev/null -w '%{http_code}' -H 'Via: 1.1 other' "$O/")"

# ---- 원본을 죽여도 캐시된 URL 은 HIT ----
kill -KILL "$O_PID"; wait "$O_PID" 2>/dev/null
check "원본 종료 후에도 캐시 HIT" HIT "$(xcache "$O/")"
check "원본 종료 후 캐시 HIT 본문" "origin-index" "$(body "$O/")"
check "원본 종료 후 캐시에 없는 URL → 502" 502 \
    "$(curl -s -x "$PX" -o /dev/null -w '%{http_code}' "$O/never-cached")"

# ---- 포워드 프록시를 켜도 일반(origin-form) 요청은 그대로 ----
check "origin-form 요청은 프록시 자신의 정적 파일로 (404)" 404 "$(status_of /nope)"

stop_server
check_contains "종료 시 캐시 통계 로그" "cache: hits=" "$(cat "$SERVER_LOG")"
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"
finish
