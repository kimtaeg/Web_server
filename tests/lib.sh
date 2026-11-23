# Shared helpers for the shell test scripts: start/stop the server and backends.
BUILD=${1:-build}
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT_DIR" || exit 1

WS_PORT=${WS_PORT:-18080}
B1=19001 B2=19002 B3=19003 DEAD=19099
BASE="http://127.0.0.1:$WS_PORT"
PIDS=""
LOG_DIR=${LOG_DIR:-/tmp/cwebserv-test}
mkdir -p "$LOG_DIR"

start_backend() { # port name
    "$BUILD/echo_backend" "$1" "$2" 2>>"$LOG_DIR/backend-$1.log" &
    eval "BACKEND_$1=$!"
    PIDS="$PIDS $!"
}

wait_port() { # port
    for _ in $(seq 1 50); do
        if curl -s -o /dev/null "http://127.0.0.1:$1/" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "port $1 did not come up" >&2
    return 1
}

start_server() { # extra args...
    $WS_WRAPPER "$BUILD/webserv" -p "$WS_PORT" -b 127.0.0.1 -r www \
        --cgi /cgi-bin=cgi-bin \
        -u api=127.0.0.1:$B1,127.0.0.1:$B2 -u app=127.0.0.1:$B3 -u slow=127.0.0.1:$B3 \
        -u dead=127.0.0.1:$DEAD \
        --route-strip /api=api --route-strip /slowapi=slow --route /dead=dead \
        --route-host app.localhost=app \
        --forward-proxy --cache \
        -s "connect_any_port on" -s "proxy_timeout 1000" -s "cgi_timeout 1000" \
        -s "max_body_size 1m" -s "keepalive_timeout 2" -s "request_timeout 2" \
        -s "fail_timeout 2" "$@" >"$LOG_DIR/server.log" 2>&1 &
    SERVER_PID=$!
    wait_port "$WS_PORT"
}

stop_all() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
}

PASS=0 FAIL=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1)); printf '  \033[32mok\033[0m   %s\n' "$1"
    else
        FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s  (expected [%s], got [%s])\n' "$1" "$2" "$3"
    fi
}
contains() { # name needle haystack
    case "$3" in
        *"$2"*) check "$1" yes yes ;;
        *) check "$1" "contains '$2'" "$(printf '%s' "$3" | head -c 200)" ;;
    esac
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
header() { # name curl-args... -> value of header
    h=$1; shift
    curl -s -D - -o /dev/null "$@" | tr -d '\r' | awk -v h="$h" 'BEGIN{IGNORECASE=1} tolower($0) ~ "^"tolower(h)":" {sub(/^[^:]*: */,""); print; exit}'
}
