#!/usr/bin/env bash
# tests/leak/leakcheck.sh — 서버 실행 → 여러 종류의 요청 → SIGINT(Ctrl+C) → 누수 보고.
#
#   Linux : ASan 빌드 + LeakSanitizer (detect_leaks=1). 누수/메모리 오류 모두 한 번에.
#   macOS : arm64 macOS 의 ASan 은 LeakSanitizer 를 지원하지 않는다.
#           → (1) debug 빌드를 macOS 기본 도구 `leaks --atExit` 로 실행해 누수 보고
#             (2) 같은 시나리오를 ASan 빌드로 실행해 메모리 오류(UAF, overflow 등)가 없는지 확인
#
# 사용: bash tests/leak/leakcheck.sh <asan 바이너리> <debug 바이너리>
set -u
ASAN_BIN=${1:-build/asan/webserver}
DEBUG_BIN=${2:-build/debug/webserver}
source "$(dirname "$0")/../func/lib.sh"

WWW="$TMP/www"
mkdir -p "$WWW/docs"
printf '<h1>home</h1>' >"$WWW/index.html"
printf 'docs' >"$WWW/docs/index.html"
head -c 500000 /dev/urandom >"$WWW/big.bin"

# 서버가 처리하는 거의 모든 경로를 한 번씩 밟는다 (정상/오류/keep-alive/중간 끊김).
traffic() {
    curl -s -o /dev/null "$(url /)"
    curl -s -o /dev/null "$(url /big.bin)"
    curl -s -o /dev/null -o /dev/null -o /dev/null "$(url /)" "$(url /docs/)" "$(url /nope)" # keep-alive
    curl -s -o /dev/null "$(url /docs)"                                                     # 301
    curl -s -o /dev/null --path-as-is "$(url /../etc/passwd)"                               # 403
    curl -s -o /dev/null -X POST -d x "$(url /)"                                            # 405
    curl -s -o /dev/null -H 'Host:' "$(url /)"                                              # 400
    curl -s -o /dev/null -I "$(url /)"                                                      # HEAD
    printf 'GET / HTTP/1.1\r\nHost: a\r\n\r\nGET /nope HTTP/1.1\r\nHost: a\r\n\r\n' |
        nc -w 1 127.0.0.1 "$PORT" >/dev/null 2>&1                                          # 파이프라이닝
    printf 'GARBAGE\r\n\r\n' | nc -w 1 127.0.0.1 "$PORT" >/dev/null 2>&1                     # 파싱 오류 → LINGER
    curl -s -o /dev/null --limit-rate 200k "$(url /big.bin)" &                              # 느린 클라이언트
    local slow=$!                                                                          # (EAGAIN → 쓰기 대기)
    for i in $(seq 1 30); do exec 6<>"/dev/tcp/127.0.0.1/$PORT"; exec 6>&-; done           # 빈 연결 폭주
    wait "$slow"
    exec 5<>"/dev/tcp/127.0.0.1/$PORT"; printf 'GET / HTTP/1.1\r\nHo' >&5; exec 5>&-     # 중간 끊김
    exec 5<>"/dev/tcp/127.0.0.1/$PORT"                                                    # 유휴 연결을 연 채로
    sleep 0.3                                                                             # SIGINT 를 맞게 한다
}

# start_server 를 쓰지 않고 직접 띄운다: leaks 가 서버를 자식으로 실행하기 때문
launch() { # launch <바이너리> <명령...>; SERVER_PID 는 신호를 보낼 "서버 프로세스" pid
    local i
    shift
    PORT=$((20000 + (RANDOM % 20000)))
    "$@" -H 127.0.0.1 -p "$PORT" -r "$WWW" -t 2000 >"$SERVER_LOG" 2>&1 &
    LAUNCH_PID=$!
    for i in $(seq 1 100); do
        curl -s -o /dev/null --max-time 1 "$(url /)" && break
        sleep 0.1
    done
    # leaks 로 감쌌다면 실제 서버는 leaks 의 자식이다
    SERVER_PID=$(pgrep -P "$LAUNCH_PID" webserver || echo "$LAUNCH_PID")
}

finish_run() { # SIGINT 후 종료 코드를 RUN_RC 에
    local target=$SERVER_PID
    kill -INT "$SERVER_PID"
    wait "$LAUNCH_PID"
    RUN_RC=$?
    exec 5>&- 2>/dev/null
    # leaks --atExit 는 검사 동안 대상 프로세스를 정지(T)시키고, 검사가 끝나도 그대로 남겨 둔다
    # (실측: 정지된 고아 프로세스가 실행마다 하나씩 쌓였다). 검사가 끝났으니 확실히 정리한다.
    if [ "$target" != "$LAUNCH_PID" ] && kill -0 "$target" 2>/dev/null; then
        kill -KILL "$target" 2>/dev/null
    fi
    SERVER_PID=""
}

if [ "$(uname -s)" = "Darwin" ]; then
    echo "[leakcheck] macOS: leaks --atExit (debug 빌드)"
    launch "$DEBUG_BIN" leaks --atExit -- "$DEBUG_BIN"
    traffic
    finish_run
    sed -n '/^Process [0-9]*: /,$p' "$SERVER_LOG" | grep -E "^Process|leaks for|total leaked|^Leak:" | head -20
    check "leaks: 종료 코드 0 (누수 없음)" 0 "$RUN_RC"
    check_contains "leaks: 0 leaks" " 0 leaks for 0 total leaked bytes" "$(cat "$SERVER_LOG")"
    check_contains "SIGINT 후 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"
    cp "$SERVER_LOG" "$TMP/leaks.log"

    echo "[leakcheck] macOS: ASan 빌드 (메모리 오류 검사)"
    launch "$ASAN_BIN" "$ASAN_BIN"
else
    echo "[leakcheck] Linux: ASan + LeakSanitizer"
    export ASAN_OPTIONS=detect_leaks=1
    launch "$ASAN_BIN" "$ASAN_BIN"
fi
traffic
finish_run
grep -E "종료 [0-9]/3|stats:" "$SERVER_LOG"
check "ASan: 종료 코드 0" 0 "$RUN_RC"
if grep -qE "ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:" "$SERVER_LOG"; then
    grep -E -A5 "ERROR: |runtime error:" "$SERVER_LOG" | head -30
    check "ASan: 오류 보고 없음" none found
else
    check "ASan: 오류 보고 없음" none none
fi
echo "--- 접근 로그 일부 ---"
grep -E '\[INFO\] [0-9.:-]+ "' "$SERVER_LOG" | head -20
finish
