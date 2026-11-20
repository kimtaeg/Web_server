#!/bin/sh
# 단위 테스트 러너: 인자로 받은 테스트 실행 파일을 차례로 실행하고 요약한다.
# 하나라도 실패(0이 아닌 종료 코드, 시그널로 죽은 경우 포함)하면 1로 종료한다.
# 사용: sh tests/run_unit.sh build/debug/tests/test_buf ...

pass=0
fail=0
failed=""

for t in "$@"; do
    name=$(basename "$t")
    echo "[$name]"
    "$t"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        pass=$((pass + 1))
    else
        if [ "$rc" -gt 128 ]; then
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
