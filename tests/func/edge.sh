#!/usr/bin/env bash
# tests/func/edge.sh — nc/printf 로 날것의 요청을 보내 경계·공격 케이스를 검증한다.
#   불완전한 헤더, 이상한 메서드, HTTP/2.0, 9KB URI, 20KB 헤더, max_body 초과, CL+TE,
#   요청 도중 끊기, slowloris(헤더를 천천히 → 헤더 마감 408), slowloris 다수 연결 후 회복
# 사용: bash tests/func/edge.sh build/release/webserver
set -u
# 서버가 닫은 소켓에 쓰면 bash 도 SIGPIPE 로 죽는다 (slowloris 테스트) → 무시하고 EPIPE 로 받는다
trap '' PIPE
BIN=${1:-build/release/webserver}
source "$(dirname "$0")/lib.sh"

WWW="$TMP/www"
mkdir -p "$WWW"
printf 'ok' >"$WWW/index.html"

# 유휴 1.5초, 헤더 마감 2초, 워커 4개
start_server "$BIN" -r "$WWW" -w 4 -q 16 -t 1500 -T 2000
echo "[edge.sh] port=$PORT"

# raw <printf 형식 문자열> → 응답 첫 줄 (CR 제거). 서버가 닫으면 nc 가 끝난다 (-w 는 안전장치).
raw() { printf "$1" | nc -w 4 127.0.0.1 "$PORT" 2>/dev/null | head -1 | tr -d '\r'; }
# raw_file <파일> → 응답 첫 줄
raw_file() { nc -w 4 127.0.0.1 "$PORT" <"$1" 2>/dev/null | head -1 | tr -d '\r'; }
alive() { check "$1 → 이후에도 서버 정상" 200 "$(status_of / --max-time 3)"; }

# ---- 이상한 메서드 / 버전 ----
check "BREW 메서드 → 501" "HTTP/1.1 501 Not Implemented" "$(raw 'BREW /pot HTTP/1.1\r\nHost: a\r\n\r\n')"
check "소문자 get → 501" "HTTP/1.1 501 Not Implemented" "$(raw 'get / HTTP/1.1\r\nHost: a\r\n\r\n')"
check "17자 메서드 → 501" "HTTP/1.1 501 Not Implemented" \
    "$(raw 'ABCDEFGHIJKLMNOPQ / HTTP/1.1\r\nHost: a\r\n\r\n')"
check "CONNECT → 501" "HTTP/1.1 501 Not Implemented" \
    "$(raw 'CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n')"
check "HTTP/2.0 → 505" "HTTP/1.1 505 HTTP Version Not Supported" \
    "$(raw 'GET / HTTP/2.0\r\nHost: a\r\n\r\n')"
check "HTTP/1.2 → 505" "HTTP/1.1 505 HTTP Version Not Supported" \
    "$(raw 'GET / HTTP/1.2\r\nHost: a\r\n\r\n')"
check "버전 없음(HTTP/0.9 형식) → 400" "HTTP/1.1 400 Bad Request" "$(raw 'GET /\r\n\r\n')"

# ---- 크기 상한 ----
{ printf 'GET /'; head -c 9216 /dev/zero | tr '\0' a; printf ' HTTP/1.1\r\nHost: a\r\n\r\n'; } >"$TMP/uri9k"
check "9KB URI → 414" "HTTP/1.1 414 URI Too Long" "$(raw_file "$TMP/uri9k")"
{ printf 'GET / HTTP/1.1\r\nHost: a\r\nX-Big: '; head -c 20480 /dev/zero | tr '\0' b; printf '\r\n\r\n'; } >"$TMP/hdr20k"
check "20KB 헤더 → 431" "HTTP/1.1 431 Request Header Fields Too Large" "$(raw_file "$TMP/hdr20k")"
{ printf 'GET / HTTP/1.1\r\nHost: a\r\n'; for i in $(seq 1 101); do printf 'X-%d: v\r\n' "$i"; done; printf '\r\n'; } >"$TMP/hdr101"
check "헤더 101개 → 431" "HTTP/1.1 431 Request Header Fields Too Large" "$(raw_file "$TMP/hdr101")"
# 본문은 보내지 않는다: 413 은 헤더를 보는 즉시 나가야 한다 (2MB 를 받고 나서가 아니라)
check "max_body(1MB) 초과 CL → 413 (본문 전송 전에)" "HTTP/1.1 413 Content Too Large" \
    "$(raw 'POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2097152\r\n\r\n')"

# ---- 스머글링 / 형식 ----
check "CL + TE 동시 → 400" "HTTP/1.1 400 Bad Request" \
    "$(raw 'POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n')"
check "TE: chunked 만 → 411" "HTTP/1.1 411 Length Required" \
    "$(raw 'POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n')"
check "서로 다른 CL 두 개 → 400" "HTTP/1.1 400 Bad Request" \
    "$(raw 'POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab')"
check "Host 없음 (1.1) → 400" "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "bare LF → 400" "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\nHost: a\n\n')"
check "헤더 값에 NUL → 400" "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n')"
check "obs-fold → 400" "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\n  2\r\n\r\n')"
check "공백 낀 헤더 이름 → 400" "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\nHost : a\r\n\r\n')"

# ---- 파이프라이닝: 한 번에 3개 → 순서대로 3개 응답 ----
n=$(printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\nGET /nope HTTP/1.1\r\nHost: a\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n' |
    nc -w 4 127.0.0.1 "$PORT" | grep -ao 'HTTP/1\.1 [0-9][0-9][0-9]' | tr '\n' '|')
# (본문 "ok" 뒤에 줄바꿈이 없어 다음 상태줄이 같은 줄에 붙으므로 위치와 무관하게 찾는다)
check "파이프라이닝 3개 순서대로" "HTTP/1.1 200|HTTP/1.1 404|HTTP/1.1 200|" "$n"

# ---- 불완전한 헤더 후 대기 → 유휴 타임아웃(1.5초)에 응답 없이 종료 ----
t0=$SECONDS
out=$(printf 'GET / HTTP/1.1\r\nHost: a\r\n' | nc -w 6 127.0.0.1 "$PORT" 2>/dev/null)
el=$((SECONDS - t0))
check "불완전한 헤더 → 응답 없이 종료" "" "$out"
if [ "$el" -le 4 ]; then check "불완전한 헤더 → 타임아웃 근처에서 닫힘 (<=4s)" yes yes
else check "불완전한 헤더 → 타임아웃 근처에서 닫힘 (<=4s)" yes "no(${el}s)"; fi
alive "불완전한 헤더"

# ---- 요청 도중 끊기 ----
for i in 1 2 3 4 5 6 7 8; do # 워커 수(4)보다 많이
    exec 5<>"/dev/tcp/127.0.0.1/$PORT"
    printf 'POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\npartial' >&5
    exec 5>&-
done
alive "요청 도중 끊기 x8"

# ---- slowloris: 헤더를 0.3초에 1바이트씩 → idle(1.5초)에는 안 걸리고 헤더 마감(2초)에 408 ----
# $'..' : \r\n 을 실제 CR/LF 로 (작은따옴표면 '\' 'r' 두 글자가 된다)
slow_req=$'GET / HTTP/1.1\r\nHost: a\r\nX-Slow: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n'
exec 5<>"/dev/tcp/127.0.0.1/$PORT"
t0=$SECONDS
i=0
while [ $i -lt 40 ]; do # 최대 12초 동안 천천히
    # 서브셸에서 쓴다: bash 내장 printf 는 쓰기 실패(EPIPE) 시 버퍼를 비우지 않아,
    # 남은 글자가 나중의 다른 출력(예: 아래 $(...))에 섞여 나온다
    ( printf '%s' "${slow_req:$i:1}" >&5 ) 2>/dev/null || break
    i=$((i + 1))
    sleep 0.3
    [ $((SECONDS - t0)) -ge 6 ] && break
done
line=$(head -1 <&5 2>/dev/null | tr -d '\r')
el=$((SECONDS - t0))
exec 5>&- 2>/dev/null
check "slowloris → 408" "HTTP/1.1 408 Request Timeout" "$line"
if [ "$el" -le 4 ]; then check "slowloris 는 헤더 마감(2초) 근처에서 끊김" yes yes
else check "slowloris 는 헤더 마감(2초) 근처에서 끊김" yes "no(${el}s)"; fi

# ---- slowloris 연결이 워커 수(4)만큼: 그동안 정상 요청은 막히지만, 마감 뒤 회복된다 ----
# (연결당 워커 하나 모델의 한계: README "한계" 참고. 헤더 마감이 피해 시간을 상한한다)
pids=""
for k in 1 2 3 4; do
    (
        exec 6<>"/dev/tcp/127.0.0.1/$PORT"
        for j in $(seq 1 30); do (printf 'X' >&6) 2>/dev/null || exit 0; sleep 0.3; done
    ) &
    pids="$pids $!"
done
sleep 0.5
t0=$SECONDS
code=$(status_of / --max-time 8)
el=$((SECONDS - t0))
check "slowloris x4 중에도 결국 200 (마감 후 워커 회복)" 200 "$code"
echo "    (slowloris x4 동안 정상 요청 대기 시간: 약 ${el}초)"
for p in $pids; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
check_contains "접근 로그에 408 기록" '"-" 408' "$(cat "$SERVER_LOG")"

alive "모든 엣지 케이스"
stop_server
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"
finish
