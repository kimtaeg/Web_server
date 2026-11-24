#!/usr/bin/env bash
# tests/func/multiplex.sh — 이벤트 루프 판의 존재 이유를 검증한다: 연결 수가 워커 수에 묶이지 않는다.
#   1) 유휴 keep-alive 연결 1000개를 붙잡은 상태에서 새 요청이 즉시 응답되는가
#   2) 헤더를 보내다 만 연결 1000개(slowloris 형태) 중에도 즉시 응답되는가
#   3) -c(최대 연결 수) 초과분은 503 으로 거절되고, 자리가 나면 회복되는가
#   4) fd 한도가 낮으면 최대 연결 수를 스스로 줄이고(경고 로그), 폭주에도 죽지 않는가
# (threadpool/ 판에서 1)·2)를 하면 워커 수만큼의 연결이 모든 워커를 차지해 새 요청이 타임아웃까지 기다린다)
# 필요: python3 (연결을 대량으로 붙잡는 도우미)
set -u
trap '' PIPE
BIN=${1:-build/release/webserver}
source "$(dirname "$0")/lib.sh"
HOLD="$(cd "$(dirname "$0")" && pwd)/hold_conns.py"
ulimit -n 4096 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null

WWW="$TMP/www"
mkdir -p "$WWW"
printf 'ok' >"$WWW/index.html"

# hold_start <포트> <개수> [partial]: 백그라운드로 연결을 열고 다 열릴 때까지 기다린다. 결과: HOLD_OPENED
hold_start() {
    rm -f "$TMP/hold.in" "$TMP/hold.out"
    mkfifo "$TMP/hold.in"
    python3 "$HOLD" "$@" <"$TMP/hold.in" >"$TMP/hold.out" 2>&1 &
    HOLD_PID=$!
    exec 7>"$TMP/hold.in" # 쓰기 쪽을 열어 둔다: 닫으면 도우미가 끝난다
    local i
    for i in $(seq 1 200); do
        grep -q '^opened' "$TMP/hold.out" 2>/dev/null && break
        sleep 0.05
    done
    HOLD_OPENED=$(awk '/^opened/ { print $2 }' "$TMP/hold.out")
    echo "    (hold: $(head -1 "$TMP/hold.out"))"
}
hold_stop() { # 결과: HOLD_CLOSED (서버가 먼저 응답/종료한 연결 수)
    exec 7>&-
    wait "$HOLD_PID" 2>/dev/null
    HOLD_CLOSED=$(awk '/^closed_by_server/ { print $2 }' "$TMP/hold.out")
}
fast() { # fast <이름>: 200 이고 1초 안에 응답했는가
    local v
    v=$(curl -s -o /dev/null -w '%{http_code} %{time_total}' --max-time 8 "$(url /)")
    check "$1 → 200" 200 "${v%% *}"
    case "${v#* }" in
    0.*) check "$1 → 1초 안에 응답" yes yes ;;
    *) check "$1 → 1초 안에 응답" yes "no(${v#* }s)" ;;
    esac
}

start_server "$BIN" -r "$WWW" -t 30000 -T 30000
echo "[multiplex.sh] port=$PORT"

# ---- 1) 유휴 연결 1000개 ----
hold_start "$PORT" 1000
if [ "${HOLD_OPENED:-0}" -ge 1000 ]; then check "유휴 연결 1000개 열림" yes yes
else check "유휴 연결 1000개 열림" yes "no(${HOLD_OPENED:-0}, ulimit -n 확인)"; fi
fast "유휴 연결 1000개 중 새 요청"
hold_stop

# ---- 2) 헤더를 보내다 만 연결 1000개 ----
hold_start "$PORT" 1000 partial
fast "요청 중간 연결 1000개 중 새 요청"
hold_stop
check "요청 중간 연결은 서버가 닫지 않음 (마감 전)" 0 "${HOLD_CLOSED:-x}"
stop_server
check_contains "최대 동시 연결 수가 1000 이상으로 기록됨" "peak_conns=10" "$(cat "$SERVER_LOG")"

# ---- 3) 최대 연결 수 50 에 100개 → 50개는 503 ----
start_server "$BIN" -r "$WWW" -c 50 -t 30000
hold_start "$PORT" 100
sleep 0.3
check "최대 연결 수 초과 상태에서 새 요청 → 503" 503 "$(status_of / --max-time 3)"
hold_stop
check "초과분 50개는 서버가 503 으로 거절" 50 "${HOLD_CLOSED:-x}"
eventually "연결을 놓으면 다시 200" 200 status_of / --max-time 1
stop_server
check_contains "busy503 통계" "busy503=" "$(cat "$SERVER_LOG")"

# ---- 4) fd 한도 64: 최대 연결 수를 스스로 줄이고, 300개 폭주에도 생존 ----
(ulimit -n 64 && exec "$BIN" -H 127.0.0.1 -p $((PORT + 1)) -r "$WWW" -t 30000) >"$TMP/low.log" 2>&1 &
LOW_PID=$!
EXTRA_PIDS="$EXTRA_PIDS $LOW_PID"
for i in $(seq 1 50); do curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$((PORT + 1))/" && break; sleep 0.1; done
check_contains "fd 한도에 맞춰 최대 연결 수 축소 (경고 로그)" "최대 연결 수를" "$(cat "$TMP/low.log")"
hold_start $((PORT + 1)) 300
hold_stop
v=""
for i in $(seq 1 20); do
    v=$(curl -s -o /dev/null -w '%{http_code}' --max-time 1 "http://127.0.0.1:$((PORT + 1))/")
    [ "$v" = 200 ] && break
    sleep 0.1
done
check "fd 한도 근처 폭주 후에도 200" 200 "$v"
kill -INT "$LOW_PID"; wait "$LOW_PID" 2>/dev/null
check_contains "fd 한도 서버도 정상 종료" "정상 종료" "$(cat "$TMP/low.log")"

finish
