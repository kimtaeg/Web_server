#!/usr/bin/env bash
# tests/load/compare.sh — 스레드 풀 판 vs 이벤트 루프 판을 같은 조건으로 비교한다 (정적 파일).
#   표 1: 처리량 — wrk -c 10/100/500/2000 (모든 연결이 쉬지 않고 요청)
#   표 2: 유휴 연결 1000개를 붙잡아 둔 상태에서 wrk -c 10 — 연결 모델의 차이가 드러나는 상황
# 결과는 docs/benchmark.md 의 "두 구현 비교" 절(표시 주석 사이)을 교체한다.
# 사용: (저장소 루트에서) bash tests/load/compare.sh [초(기본 8)]   또는 make load
# 필요: wrk, python3
set -u
DUR=${1:-8}
TP_BIN=threadpool/build/release/webserver
MX_BIN=multiplex/build/release/webserver
OUT=docs/benchmark.md
CONNS="10 100 500 2000"
HOLD=multiplex/tests/func/hold_conns.py
command -v wrk >/dev/null || { echo "wrk 가 필요하다 (brew install wrk)"; exit 1; }
[ -x "$TP_BIN" ] && [ -x "$MX_BIN" ] || { echo "먼저 빌드: make all"; exit 1; }
ulimit -n 16384 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null
source "$(dirname "$0")/../../threadpool/tests/func/lib.sh"

mkdir -p "$TMP/www"
head -c 1024 /dev/zero | tr '\0' 'x' >"$TMP/www/index.html" # 1KB 정적 파일

# 서버 구성: 이름|바이너리|옵션
SERVERS="스레드풀 (워커 8)|$TP_BIN|-w 8 -q 1024
스레드풀 (워커 64)|$TP_BIN|-w 64 -q 1024
이벤트루프 (1 스레드)|$MX_BIN|-c 8192"

row() { # row <서버> <연결> <wrk 출력> → 표 한 행 (마지막 열은 호출자가)
    awk -v s="$1" -v c="$2" '
        /^ +Latency / && !lat { lat = $2 }
        /^ +99%/ { p99 = $2 }
        /Socket errors:/ { gsub(",", ""); se = "c" $4 " r" $6 " w" $8 " t" $10 }
        /Non-2xx/ { n2 = $5 }
        /Requests\/sec:/ { rps = $2 }
        END {
            if (se == "") se = "0"
            if (n2 == "") n2 = 0
            printf "| %s | %s | %s | %s | %s | %s | %s |", s, c, rps, lat, p99, se, n2
        }' "$3"
}
codes() { # 측정 구간 동안 서버 접근 로그의 상태 코드 분포
    tail -n +"$2" "$1" | grep -oE '" [0-9]{3} ' | sort | uniq -c |
        awk '{ printf "%s%s:%s", (n++ ? " " : ""), $3, $1 }'
}
cooldown() { # 앞 실행의 TIME_WAIT 가 빠질 때까지 (최대 70초)
    local i
    for i in $(seq 1 70); do
        [ "$(netstat -an 2>/dev/null | grep -c TIME_WAIT)" -lt 1000 ] && return 0
        sleep 1
    done
}
measure() { # measure <서버 이름> <연결 수> <로그> [표에 쓸 연결 라벨] [nocool]
    local start threads=4 label=${4:-$2}
    [ "$2" -lt 4 ] && threads=$2
    [ "${5:-}" = nocool ] || cooldown
    wrk -t"$threads" -c"$2" -d1s "$(url /index.html)" >/dev/null 2>&1 # 워밍업
    start=$(($(wc -l <"$3") + 1))
    wrk -t"$threads" -c"$2" -d"${DUR}s" --latency --timeout 2s "$(url /index.html)" >"$TMP/wrk.out" 2>&1
    sleep 0.3
    if grep -q 'Requests/sec: *0\.00' "$TMP/wrk.out"; then # 응답이 하나도 없으면 wrk 는 지연값을 0 으로 찍는다
        echo "| $1 | $label | **0** | - | - | 측정 구간 내 응답 0건 | - | - |"
        return
    fi
    { row "$1" "$label" "$TMP/wrk.out"; echo " $(codes "$3" "$start") |"; }
}

T1="$TMP/t1.md"
T2="$TMP/t2.md"
: >"$T1"
: >"$T2"
while IFS='|' read -r name bin opts; do
    # shellcheck disable=SC2086
    start_server "$bin" -r "$TMP/www" -t 5000 $opts
    for c in $CONNS; do
        measure "$name" "$c" "$SERVER_LOG" | tee -a "$T1"
    done

    stop_server

    # 표 2: 유휴 연결 1000개 + wrk -c 10.
    # 유휴 타임아웃을 60초로 따로 띄운다: 기본 5초면 측정 전에 서버가 유휴 연결을 닫아 버려 상황이 성립하지 않는다.
    # 같은 이유로 연결을 연 뒤에는 cooldown(최대 70초)을 하지 않는다 (처음 측정에서 이 실수로 표 2 가 무의미했다).
    cooldown
    # shellcheck disable=SC2086
    start_server "$bin" -r "$TMP/www" -t 60000 $opts
    rm -f "$TMP/hold.in"
    mkfifo "$TMP/hold.in"
    python3 "$HOLD" "$PORT" 1000 <"$TMP/hold.in" >"$TMP/hold.out" 2>&1 &
    hp=$!
    exec 7>"$TMP/hold.in"
    for i in $(seq 1 200); do grep -q '^opened' "$TMP/hold.out" 2>/dev/null && break; sleep 0.05; done
    held=$(awk '/^opened/ { print $2 }' "$TMP/hold.out")
    measure "$name" 10 "$SERVER_LOG" "10 (+유휴 ${held:-?})" nocool | tee -a "$T2"
    exec 7>&-
    wait "$hp" 2>/dev/null
    stop_server
done <<EOF
$SERVERS
EOF

# ---- docs/benchmark.md 의 비교 절 교체 ----
SECTION="$TMP/section.md"
{
    echo "<!-- compare:start (tests/load/compare.sh 가 생성) -->"
    echo "## 두 구현 비교: 스레드 풀 vs 이벤트 루프"
    echo
    echo "\`bash tests/load/compare.sh $DUR\` 로 생성 ($(date '+%Y-%m-%d'))."
    echo
    echo "- 환경: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || grep -m1 'model name' /proc/cpuinfo | cut -d: -f2) /" \
        "$(uname -sr) / $(wrk --version 2>&1 | head -1 | cut -d' ' -f1-2) / 루프백(127.0.0.1)"
    echo "- 부하: wrk -t4 -c<연결> -d${DUR}s --timeout 2s (1초 워밍업 후 측정), 1KB 정적 파일, keep-alive"
    echo "- 공통: 유휴 타임아웃 5초, 연결당 최대 100요청. 스레드 풀은 대기 큐 1024"
    echo "- 소켓 오류 timeout = wrk 가 2초 안에 응답을 못 받은 요청 수"
    echo "- 스레드 풀의 non-2xx 는 큐 포화 503 이다. main 스레드가 바로 거절하므로 접근 로그(마지막 열)에는 남지 않는다"
    echo "- 1회 측정이다. 같은 조건을 다시 재면 ±50% 까지 흔들리는 것을 확인했다 (노트북, 다른 프로세스 영향). 경향만 읽을 것"
    echo
    echo "### 표 1. 모든 연결이 쉬지 않고 요청할 때"
    echo
    echo "| 서버 | 연결 | req/s | 평균 지연 | p99 | 소켓 오류 (c/r/w/timeout) | non-2xx | 서버 로그 상태 코드 |"
    echo "|---|---:|---:|---:|---:|---|---:|---|"
    cat "$T1"
    echo
    echo "### 표 2. 유휴 keep-alive 연결 1000개를 붙잡아 둔 상태에서 wrk -c 10"
    echo
    echo "유휴 연결은 요청을 보내지 않고 연결만 유지한다 (브라우저의 keep-alive 연결, 느린 모바일 클라이언트 등)."
    echo "이 표만 유휴 타임아웃을 60초로 둔다 (측정 중에 서버가 유휴 연결을 닫지 않도록)."
    echo
    echo "| 서버 | 연결 | req/s | 평균 지연 | p99 | 소켓 오류 (c/r/w/timeout) | non-2xx | 서버 로그 상태 코드 |"
    echo "|---|---:|---:|---:|---:|---|---:|---|"
    cat "$T2"
    echo "<!-- compare:end -->"
} >"$SECTION"

if grep -q '<!-- compare:start' "$OUT" 2>/dev/null; then
    awk -v f="$SECTION" '
        /<!-- compare:start/ { while ((getline line < f) > 0) print line; skip = 1; next }
        /<!-- compare:end -->/ { skip = 0; next }
        !skip { print }' "$OUT" >"$TMP/bench.new" && mv "$TMP/bench.new" "$OUT"
else
    { echo; cat "$SECTION"; } >>"$OUT"
fi
echo "→ $OUT"
