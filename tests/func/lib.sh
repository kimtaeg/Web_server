# tests/func/lib.sh — 기능 테스트 공용 도우미 (bash 3.2 호환: macOS 기본 bash).
# 사용: source "$(dirname "$0")/lib.sh"

PASS=0
FAIL=0
SERVER_PID=""
PORT=""
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cweb-func-XXXXXX")
SERVER_LOG="$TMP/server.log"

cleanup() {
    stop_server
    chmod -R u+rwx "$TMP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

check() { # check <이름> <기대> <실제>
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
        printf '  ok   %s\n' "$1"
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL %s (기대 [%s], 실제 [%s])\n' "$1" "$2" "$3"
    fi
}

check_contains() { # check_contains <이름> <찾을 문자열> <대상>
    case "$3" in
    *"$2"*) PASS=$((PASS + 1)); printf '  ok   %s\n' "$1" ;;
    *) FAIL=$((FAIL + 1)); printf '  FAIL %s ([%s] 없음)\n' "$1" "$2" ;;
    esac
}

# start_server <바이너리> <추가 옵션...>: 빈 포트를 찾아 띄우고 응답할 때까지 기다린다.
start_server() {
    local bin=$1 try
    shift
    for try in 1 2 3 4 5; do
        PORT=$((20000 + (RANDOM % 20000)))
        "$bin" -H 127.0.0.1 -p "$PORT" "$@" >"$SERVER_LOG" 2>&1 &
        SERVER_PID=$!
        local i
        for i in $(seq 1 50); do
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                break # bind 실패 등으로 죽음 → 다른 포트로 재시도
            fi
            if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/" 2>/dev/null; then
                return 0
            fi
            sleep 0.1
        done
        stop_server
    done
    echo "서버를 띄우지 못함. 로그:"
    cat "$SERVER_LOG"
    exit 1
}

stop_server() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID"
        wait "$SERVER_PID" 2>/dev/null
    fi
    SERVER_PID=""
}

url() { echo "http://127.0.0.1:$PORT$1"; }

status_of() { # status_of <경로> [curl 옵션...]
    local p=$1
    shift
    curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" "$(url "$p")"
}

finish() {
    echo "==== $(basename "$0"): $PASS passed, $FAIL failed ===="
    [ "$FAIL" -eq 0 ]
}
