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
    # 워치독: 출력을 /dev/null 로 돌려서, 남은 sleep 이 파이프를 붙잡지 않게 한다
    ( sleep "$limit"; kill -TERM "$pid" ) >/dev/null 2>&1 &
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
