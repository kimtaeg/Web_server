#!/usr/bin/env bash
# tests/load/run.sh — wrk 로 동시 연결 × 워커 수 조합을 측정해 docs/benchmark.md 표를 만든다.
#   대상: (1) 정적 파일  (2) 리버스 프록시 → 백엔드 2개(라운드로빈)
#   조합: -c 10/100/500 × 워커 4/8/16
# 사용: bash tests/load/run.sh [초(기본 8)] [바이너리(기본 threadpool/build/release/webserver)]
# 저장소 루트에서 실행한다 (make load)
# 필요: wrk (macOS: brew install wrk / Ubuntu: apt install wrk)
set -u
DUR=${1:-8}
BIN=${2:-threadpool/build/release/webserver}
OUT=docs/benchmark.md
CONNS="10 100 500"
WORKERS="4 8 16"
command -v wrk >/dev/null || { echo "wrk 가 필요하다 (brew install wrk)"; exit 1; }
ulimit -n 8192 2>/dev/null
source "$(dirname "$0")/../../threadpool/tests/func/lib.sh"

mkdir -p "$TMP/www"
head -c 1024 /dev/zero | tr '\0' 'x' >"$TMP/www/index.html" # 1KB 정적 파일

# 백엔드 2개 (리버스 프록시 측정용, 워커 16 고정)
spawn_server 0 "$TMP/b1.log" "$BIN" -r "$TMP/www" -w 16 -q 1024 --name A
B1=$SPAWN_PORT
spawn_server 0 "$TMP/b2.log" "$BIN" -r "$TMP/www" -w 16 -q 1024 --name B
B2=$SPAWN_PORT
cat >"$TMP/proxy.conf" <<EOF
root $TMP/www
route / 127.0.0.1:$B1 127.0.0.1:$B2
EOF

# wrk 출력에서 필요한 값만 뽑아 표의 한 행으로
row() { # row <대상> <워커> <연결> <wrk 출력 파일>
    awk -v t="$1" -v w="$2" -v c="$3" '
        /^ +Latency / && !lat { lat = $2 }
        /^ +99%/ { p99 = $2 }
        /Socket errors:/ { gsub(",", ""); se = "c" $4 " r" $6 " w" $8 " t" $10 }
        /Non-2xx/ { n2 = $5 }
        /Requests\/sec:/ { rps = $2 }
        END {
            if (se == "") se = "0"
            if (n2 == "") n2 = 0
            printf "| %s | %s | %s | %s | %s | %s | %s | %s |", t, w, c, rps, lat, p99, se, n2
        }' "$4"
}

# 측정 구간 동안 서버 접근 로그에 찍힌 상태 코드 분포 (wrk 는 non-2xx 를 코드별로 나눠 주지 않는다)
codes() { # codes <로그> <시작 줄 번호>
    tail -n +"$2" "$1" | grep -oE '" [0-9]{3} ' | sort | uniq -c |
        awk '{ printf "%s%s:%s", (n++ ? " " : ""), $3, $1 }' # uniq -c 출력: <개수> " <코드>
}

ROWS="$TMP/rows.md"
: >"$ROWS"
# 직전 실행이 남긴 TIME_WAIT 소켓이 빠질 때까지 기다린다 (최대 70초).
# 리버스 프록시는 요청마다 업스트림 연결을 새로 열어 임시 포트를 소모하므로, 이 대기 없이 돌리면
# 앞 실행의 TIME_WAIT 때문에 다음 실행이 시작부터 포트 고갈 상태가 된다.
cooldown() {
    local i n
    for i in $(seq 1 70); do
        n=$(netstat -an 2>/dev/null | grep -c TIME_WAIT)
        [ "$n" -lt 1000 ] && return 0
        sleep 1
    done
}

run_one() { # run_one <대상> <워커> <연결> <URL> <서버 로그>
    local threads=4 start
    cooldown
    [ "$3" -lt 4 ] && threads=$3
    wrk -t"$threads" -c"$3" -d1s "$4" >/dev/null 2>&1 # 워밍업
    start=$(($(wc -l <"$5") + 1))
    wrk -t"$threads" -c"$3" -d"${DUR}s" --latency --timeout 2s "$4" >"$TMP/wrk.out" 2>&1
    sleep 0.3
    { row "$1" "$2" "$3" "$TMP/wrk.out"; echo " $(codes "$5" "$start") |"; } | tee -a "$ROWS"
}

echo "| 대상 | 워커 | 연결 | req/s | 평균 지연 | p99 | 소켓 오류 (connect/read/write/timeout) | non-2xx | 서버 로그 상태 코드 |"
for w in $WORKERS; do
    : >"$TMP/s.log"
    spawn_server 0 "$TMP/s.log" "$BIN" -r "$TMP/www" -w "$w" -q 1024
    for c in $CONNS; do run_one 정적 "$w" "$c" "http://127.0.0.1:$SPAWN_PORT/index.html" "$TMP/s.log"; done
    kill -INT "$SPAWN_PID"; wait "$SPAWN_PID" 2>/dev/null
done
for w in $WORKERS; do
    : >"$TMP/p.log"
    spawn_server 0 "$TMP/p.log" "$BIN" -f "$TMP/proxy.conf" -w "$w" -q 1024
    for c in $CONNS; do run_one 리버스프록시 "$w" "$c" "http://127.0.0.1:$SPAWN_PORT/index.html" "$TMP/p.log"; done
    kill -INT "$SPAWN_PID"; wait "$SPAWN_PID" 2>/dev/null
done

# ---- docs/benchmark.md ----
{
    echo "# 부하 테스트 결과"
    echo
    echo "\`bash tests/load/run.sh $DUR\` 로 생성 ($(date '+%Y-%m-%d'))."
    echo
    echo "- 환경: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || grep -m1 'model name' /proc/cpuinfo | cut -d: -f2) /" \
        "$(uname -sr) / $(wrk --version 2>&1 | head -1 | cut -d' ' -f1-2) / 루프백(127.0.0.1)"
    echo "- 부하: wrk -t4 -c<연결> -d${DUR}s --timeout 2s (1초 워밍업 후 측정), 1KB 정적 파일, keep-alive"
    echo "- 서버: 대기 큐 1024, 유휴 타임아웃 5초, 연결당 최대 100요청 (기본값)"
    echo "- 리버스 프록시: 프록시(워커 수 변동) → 백엔드 2개(각 워커 16) 라운드로빈, 업스트림은 요청마다 새 연결"
    echo "- 소켓 오류 timeout = wrk 가 2초 안에 응답을 못 받은 요청 수"
    echo "- 각 실행 전에 TIME_WAIT 가 1000개 미만이 될 때까지 대기 (앞 실행의 영향 제거)"
    echo "- 마지막 열: 측정 구간 동안 서버 접근 로그의 상태 코드별 개수 (wrk 의 non-2xx 를 코드별로 나눈 것)"

    echo
    echo "| 대상 | 워커 | 연결 | req/s | 평균 지연 | p99 | 소켓 오류 (c/r/w/timeout) | non-2xx | 서버 로그 상태 코드 |"
    echo "|---|---:|---:|---:|---:|---:|---|---:|---|"
    cat "$ROWS"
} >"$OUT"
echo "→ $OUT"
