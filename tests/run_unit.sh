#!/bin/sh
# 단위 테스트 러너: 인자로 받은 테스트 실행 파일을 차례로 실행하고 요약한다.
# 하나라도 실패(0이 아닌 종료 코드, 시그널로 죽은 경우 포함)하면 1로 종료한다.
# 테스트 하나가 멈춰도 전체가 멈추지 않도록 TEST_TIMEOUT 초(기본 60)가 지나면 강제 종료한다.
# (macOS 에는 coreutils timeout 이 없어서 셸 워치독으로 구현)
# 사용: sh tests/run_unit.sh build/debug/tests/test_buf ...

limit=${TEST_TIMEOUT:-60}
pass=0
fail=0
failed=""

for t in "$@"; do
    name=$(basename "$t")
    echo "[$name]"
    "$t" &
    pid=$!
    # 워치독: 시간 초과 시 죽이기 직전에 스택을 남긴다 (멈춤 원인 분석용).
    # 출력은 파일로만 보내서, 남은 sleep 이 make 의 파이프를 붙잡지 않게 한다.
    stackfile="${TMPDIR:-/tmp}/cweb-hang-$name-$$.txt"
    (
        sleep "$limit"
        if command -v sample >/dev/null; then sample "$pid" 1 >"$stackfile" 2>&1
        elif command -v gdb >/dev/null; then gdb -batch -ex 'thread apply all bt' -p "$pid" >"$stackfile" 2>&1
        fi
        kill -TERM "$pid"
    ) >/dev/null 2>&1 &
    wd=$!
    wait "$pid"
    rc=$?
    pkill -P "$wd" >/dev/null 2>&1
    kill "$wd" >/dev/null 2>&1
    wait "$wd" 2>/dev/null
    if [ "$rc" -eq 0 ]; then
        pass=$((pass + 1))
    else
        if [ "$rc" -eq 143 ]; then
            echo "  !! $name: ${limit}초 안에 끝나지 않아 강제 종료 (멈춤 의심)"
            if [ -s "$stackfile" ]; then
                echo "  !! 멈춘 시점의 스택 (테스트·소스 프레임만, 전체: $stackfile):"
                grep -E 'test_|src/|\.c:[0-9]+' "$stackfile" | head -30 | sed 's/^/     /'
            fi
        elif [ "$rc" -gt 128 ]; then
            echo "  !! $name: 시그널 $((rc - 128)) 로 비정상 종료"
        fi
        fail=$((fail + 1))
        failed="$failed $name"
    fi
done

echo "==== unit tests: $pass passed, $fail failed ===="
if [ "$fail" -ne 0 ]; then
    echo "failed:$failed"
    exit 1
fi
