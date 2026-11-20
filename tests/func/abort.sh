#!/usr/bin/env bash
# tests/func/abort.sh — 클라이언트가 요청 중간에 끊어도 워커와 fd 가 회수되는지 검증한다.
#
# 설정: 워커 1개, 큐 4, 유휴 타임아웃 30초.
#   워커가 끊긴 연결을 회수하지 못하면 30초 동안 그 연결에 붙잡혀 있게 되고,
#   하나뿐인 워커가 막히므로 뒤이은 curl(3초 제한)이 실패한다.
#   → "끊긴 직후 curl 이 바로 성공" = 워커가 EOF 를 감지하고 즉시 풀려났다는 증거.
# fd 회수는 lsof 로 센 서버 프로세스의 fd 개수가 처음과 같아지는지로 확인한다.
set -u
BIN=${1:-build/release/webserver}
source "$(dirname "$0")/lib.sh"

WWW="$TMP/www"
mkdir -p "$WWW"
printf 'ok' >"$WWW/index.html"

start_server "$BIN" -r "$WWW" -w 1 -q 4 -t 30000
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
check "nc 헤더 중간 끊김 x10 직후 → 워커 즉시 응답" 200 "$(status_of / --max-time 3)"

# 2) 본문 중간에 끊기 (Content-Length 100 인데 10바이트만)
for i in $(seq 1 10); do
    printf 'POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\n0123456789' |
        nc -w 1 127.0.0.1 "$PORT" >/dev/null 2>&1 &
    NC_PIDS="$NC_PIDS $!"
done
wait_nc
check "nc 본문 중간 끊김 x10 직후 → 워커 즉시 응답" 200 "$(status_of / --max-time 3)"

# 3) nc 대기 없이 즉시 끊기 (bash /dev/tcp: fd 를 닫으면 바로 FIN)
for i in $(seq 1 20); do
    exec 5<>"/dev/tcp/127.0.0.1/$PORT"
    printf 'GET /index.html HTTP/1.1\r\nHo' >&5
    exec 5>&-
done
check "즉시 끊김 x20 직후 → 워커 즉시 응답" 200 "$(status_of / --max-time 3)"

# 4) 연결만 하고 아무것도 안 보낸 채 끊기
for i in $(seq 1 20); do
    exec 5<>"/dev/tcp/127.0.0.1/$PORT"
    exec 5>&-
done
check "빈 연결 x20 직후 → 워커 즉시 응답" 200 "$(status_of / --max-time 3)"

sleep 0.5 # 마지막 연결의 lingering close 대기
check "서버 fd 개수가 처음과 같음 (fd 누수 없음)" "$base" "$(fd_count)"
if kill -0 "$SERVER_PID" 2>/dev/null; then check "서버 생존" alive alive
else check "서버 생존" alive dead; fi

stop_server
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"
finish
