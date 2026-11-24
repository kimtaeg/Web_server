#!/usr/bin/env bash
# tests/func/abort.sh — 클라이언트가 요청 중간에 끊어도 연결 상태와 fd 가 회수되는지 검증한다.
#
# 설정: 유휴 타임아웃 30초.
#   이벤트 루프는 끊긴 연결 때문에 다른 요청이 막히지는 않는다 (threadpool/ 판과 다른 점).
#   대신 EOF 를 놓치면 끊긴 연결이 30초 동안 fd 와 메모리를 차지한 채 남는다.
#   → lsof 로 센 서버 fd 개수가 처음과 같아지는지 = 루프가 EOF 를 보고 즉시 정리했다는 증거.
set -u
BIN=${1:-build/release/webserver}
source "$(dirname "$0")/lib.sh"

WWW="$TMP/www"
mkdir -p "$WWW"
printf 'ok' >"$WWW/index.html"

start_server "$BIN" -r "$WWW" -t 30000
echo "[abort.sh] port=$PORT"

# 주의: 인자 없는 wait 는 백그라운드로 띄운 서버까지 기다려 영원히 멈춘다 → nc pid 만 기다린다
NC_PIDS=""
wait_nc() { local p; for p in $NC_PIDS; do wait "$p" 2>/dev/null; done; NC_PIDS=""; }

fd_count() { lsof -p "$SERVER_PID" 2>/dev/null | tail -n +2 | wc -l | tr -d ' '; }

sleep 0.3 # start_server 의 확인용 curl 연결이 정리될 시간
base=$(fd_count)

# 1) nc 로 헤더 일부만 보내고 끊기 (nc -w 1: 입력이 끝나고 1초 뒤 연결 종료)
for i in $(seq 1 10); do
    printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Ag' | nc -w 1 127.0.0.1 "$PORT" >/dev/null 2>&1 &
    NC_PIDS="$NC_PIDS $!"
done
wait_nc
check "nc 헤더 중간 끊김 x10 직후 → 즉시 응답" 200 "$(status_of / --max-time 3)"

# 2) 본문 중간에 끊기 (Content-Length 100 인데 10바이트만)
for i in $(seq 1 10); do
    printf 'POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\n0123456789' |
        nc -w 1 127.0.0.1 "$PORT" >/dev/null 2>&1 &
    NC_PIDS="$NC_PIDS $!"
done
wait_nc
check "nc 본문 중간 끊김 x10 직후 → 즉시 응답" 200 "$(status_of / --max-time 3)"

# 3) nc 대기 없이 즉시 끊기 (bash /dev/tcp: fd 를 닫으면 바로 FIN)
for i in $(seq 1 20); do
    exec 5<>"/dev/tcp/127.0.0.1/$PORT"
    printf 'GET /index.html HTTP/1.1\r\nHo' >&5
    exec 5>&-
done
check "즉시 끊김 x20 직후 → 즉시 응답" 200 "$(status_of / --max-time 3)"

# 4) 연결만 하고 아무것도 안 보낸 채 끊기
for i in $(seq 1 20); do
    exec 5<>"/dev/tcp/127.0.0.1/$PORT"
    exec 5>&-
done
check "빈 연결 x20 직후 → 즉시 응답" 200 "$(status_of / --max-time 3)"

sleep 0.5 # 마지막 연결의 lingering close 대기
check "서버 fd 개수가 처음과 같음 (fd 누수 없음)" "$base" "$(fd_count)"
if kill -0 "$SERVER_PID" 2>/dev/null; then check "서버 생존" alive alive
else check "서버 생존" alive dead; fi

stop_server
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"
finish
