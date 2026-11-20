#!/usr/bin/env bash
# tests/valgrind/run.sh — valgrind(memcheck)로 단위 테스트와 기능 테스트 서버를 검사한다. (Linux 전용)
#
# 판정 기준 (계획에서 정한 것): valgrind 가 보고한 메모리 오류 0, definitely lost 0.
#   valgrind 아래에서는 서버가 수십 배 느려져 "504 가 3초 안에" 같은 시간 검사는 실패할 수 있다.
#   그런 기능 검사 결과는 참고용으로 출력만 하고 통과 기준에서 뺀다.
set -u
command -v valgrind >/dev/null || { echo "valgrind 가 없다 (Linux: apt install valgrind)"; exit 1; }
ROOT=$(pwd)
VG_LOGS=$(mktemp -d "${TMPDIR:-/tmp}/cweb-vg-XXXXXX")
VG="valgrind --tool=memcheck --leak-check=full --errors-for-leak-kinds=definite --show-leak-kinds=definite --error-exitcode=99 --trace-children=no"
fail=0

echo "== 1. 단위 테스트 (debug 빌드) =="
for t in build/debug/tests/test_*; do
    [ -x "$t" ] || continue
    case "$t" in *.d|*.dSYM) continue ;; esac
    name=$(basename "$t")
    # %p: 테스트가 fork 하는 자식(CGI 등)의 로그가 섞이지 않게 pid 별 파일로
    $VG --log-file="$VG_LOGS/unit-$name.%p.log" "$t" >"$VG_LOGS/unit-$name.out" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  ok   $name"
    elif [ "$rc" -eq 99 ]; then # --error-exitcode: valgrind 가 메모리 오류/누수를 찾음
        echo "  FAIL $name (valgrind 오류)"
        fail=1
    else
        # 기능 테스트와 같은 기준: valgrind 아래에서만 나는 검사 실패는 참고용.
        # 원인은 둘 중 하나다 — (1) 수십 배 느려져서 시간 검사가 실패, (2) valgrind 의 동작 차이
        #   (실측: valgrind 는 execve 를 가로채므로, exec 이 실패한 자식의 종료 코드가 127 이 아니게 된다
        #    → test_cgi 의 "exec 실패 = CGI_ERR_EXEC" 검사가 CGI_ERR_EXIT 로 나옴).
        # 같은 테스트의 일반 실행 결과는 make test 가 판정한다.
        echo "  (참고) $name: valgrind 아래에서만 검사 실패 (종료 코드 $rc) — 시간 검사 또는 valgrind 동작 차이"
        grep -E 'FAIL' "$VG_LOGS/unit-$name.out" | head -5 | sed 's/^/        /'
    fi
done

echo "== 2. 기능 테스트: 서버를 valgrind 래퍼로 =="
# 래퍼를 build/valgrind/webserver 에 두고 cgi-bin 을 옆에 연결한다 (스크립트가 바이너리 옆의 cgi-bin 을 쓴다)
mkdir -p build/valgrind
ln -sfn ../release/cgi-bin build/valgrind/cgi-bin
cat >build/valgrind/webserver <<WRAP
#!/bin/sh
exec $VG --log-file=$VG_LOGS/server.%p.log $ROOT/build/debug/webserver "\$@"
WRAP
chmod +x build/valgrind/webserver
for t in tests/func/*.sh; do
    case "$t" in */lib.sh) continue ;; esac
    echo "-- $t (기능 검사 결과는 참고용)"
    bash "$t" build/valgrind/webserver 2>&1 | grep -E 'FAIL|====' || true
done

echo "== 3. 판정: valgrind 로그 =="
nlogs=$(ls "$VG_LOGS"/server.*.log 2>/dev/null | wc -l | tr -d ' ')
bad=$(grep -lE 'ERROR SUMMARY: [1-9]|definitely lost: [1-9]' "$VG_LOGS"/*.log 2>/dev/null)
echo "  서버 프로세스 로그 ${nlogs}개 검사"
if [ -n "$bad" ]; then
    for f in $bad; do
        echo "  !! $(basename "$f")"
        grep -E -B2 -A12 'Invalid|uninitialised|definitely lost in' "$f" | head -40
    done
    fail=1
else
    echo "  메모리 오류 0, definitely lost 0"
fi
echo "(로그: $VG_LOGS)"
exit $fail
