#!/usr/bin/env bash
# tests/func/static.sh — 실제 서버를 띄우고 curl 로 정적 파일 서빙을 검증한다.
# 사용: bash tests/func/static.sh build/release/webserver
set -u
BIN=${1:-build/release/webserver}
source "$(dirname "$0")/lib.sh"

# ---- 문서 루트 준비 (심볼릭 링크·권한은 실행 시 만든다: 저장소에 링크를 넣지 않기 위해) ----
WWW="$TMP/www"
mkdir -p "$WWW/docs" "$WWW/empty" "$TMP/outside"
printf '<h1>home</h1>' >"$WWW/index.html"
printf 'body{}' >"$WWW/style.css"
printf '{"a":1}' >"$WWW/data.json"
printf 'docs-index' >"$WWW/docs/index.html"
printf 'SECRET' >"$TMP/outside/secret.txt"
ln -s ../outside/secret.txt "$WWW/escape"
printf 'nope' >"$WWW/noperm.txt"
chmod 000 "$WWW/noperm.txt"
head -c 3000000 /dev/urandom >"$WWW/big.bin" # 3MB: 고정 버퍼 반복 전송 검증

start_server "$BIN" -r "$WWW" -w 4 -q 16 -t 2000

echo "[static.sh] port=$PORT"

# ---- 200 / 본문 ----
check "GET / → 200" 200 "$(status_of /)"
check "GET / 본문 = index.html" "<h1>home</h1>" "$(curl -s "$(url /)")"
check "GET /docs/ 본문" "docs-index" "$(curl -s "$(url /docs/)")"

# ---- MIME ----
check "style.css Content-Type" "text/css; charset=utf-8" \
    "$(curl -s -o /dev/null -w '%{content_type}' "$(url /style.css)")"
check "data.json Content-Type" "application/json" \
    "$(curl -s -o /dev/null -w '%{content_type}' "$(url /data.json)")"
check "index Content-Type" "text/html; charset=utf-8" \
    "$(curl -s -o /dev/null -w '%{content_type}' "$(url /)")"

# ---- 큰 파일 ----
curl -s -o "$TMP/big.down" "$(url /big.bin)"
if cmp -s "$WWW/big.bin" "$TMP/big.down"; then check "3MB 파일 내용 일치" same same
else check "3MB 파일 내용 일치" same different; fi

# ---- 404 / 301 / 403 ----
check "없는 파일 → 404" 404 "$(status_of /nope.html)"
check "/docs → 301" 301 "$(status_of /docs)"
check "/docs → Location: /docs/" "/docs/" \
    "$(curl -s -o /dev/null -w '%{redirect_url}' "$(url /docs)" | sed "s|http://127.0.0.1:$PORT||")"
check "index 없는 디렉터리 → 403" 403 "$(status_of /empty/)"
check "../ 탈출 → 403" 403 "$(status_of /../outside/secret.txt --path-as-is)"
check "%2e%2e 탈출 → 403" 403 "$(status_of /%2e%2e/outside/secret.txt --path-as-is)"
check "%2E%2E%2F 탈출 → 403" 403 "$(status_of /%2E%2E%2Foutside%2Fsecret.txt --path-as-is)"
check "루트 밖 심볼릭 링크 → 403" 403 "$(status_of /escape)"
if [ "$(id -u)" -ne 0 ]; then
    check "읽기 권한 없음 → 403" 403 "$(status_of /noperm.txt)"
fi
body=$(curl -s --path-as-is "$(url /../outside/secret.txt)"; curl -s "$(url /escape)")
case "$body" in *SECRET*) check "탈출 시 비밀 내용 미노출" hidden leaked ;;
*) check "탈출 시 비밀 내용 미노출" hidden hidden ;; esac

# ---- 메서드 ----
check "HEAD → 200" 200 "$(status_of /style.css -I)"
check "HEAD 본문 0바이트" 0 "$(curl -s -I -o /dev/null -w '%{size_download}' "$(url /style.css)")"
check_contains "HEAD Content-Length = 실제 크기" "Content-Length: 6" "$(curl -s -I "$(url /style.css)")"
check "POST → 405" 405 "$(status_of / -X POST -d x)"
check_contains "405 Allow 헤더" "Allow: GET, HEAD" "$(curl -s -i -X POST -d x "$(url /)")"

# ---- 파서 오류가 HTTP 응답으로 ----
check "Host 없음 → 400" 400 "$(status_of / -H 'Host:')"
check "긴 URI → 414" 414 "$(status_of "/$(head -c 9000 /dev/zero | tr '\0' a)")"
check "Content-Length 초과 → 413" 413 \
    "$(status_of / -X POST -H 'Content-Length: 99999999' --data-binary '')"

# ---- keep-alive: 한 번의 curl 에서 여러 요청이 같은 연결을 재사용하는가 ----
# %{num_connects} = 그 요청을 위해 새로 연 TCP 연결 수. 재사용이면 0.
# (curl -v 의 "Re-using existing ..." 문구는 버전마다 바뀌어서 문구 대신 이 값을 쓴다)
v=$(curl -s -o /dev/null -o /dev/null -o /dev/null -w '%{num_connects}' \
    "$(url /)" "$(url /style.css)" "$(url /docs/)")
check "keep-alive: 3요청에 연결 1개 (100)" 100 "$v"
v=$(curl -sv -o /dev/null -o /dev/null "$(url /)" "$(url /style.css)" 2>&1)
check_contains "curl -v 로그에 재사용 표시" "existing" "$v"
check_contains "Connection: close 존중" "Connection: close" \
    "$(curl -s -i -H 'Connection: close' "$(url /)")"
v=$(curl -s --http1.0 -o /dev/null -o /dev/null -w '%{num_connects}' "$(url /)" "$(url /style.css)")
check "HTTP/1.0 은 요청마다 새 연결 (11)" 11 "$v"

stop_server

# ---- 503: 워커 1, 큐 1 로 띄우고 연결 2개를 붙잡아 둔다 ----
start_server "$BIN" -r "$WWW" -w 1 -q 1 -t 3000
exec 3<>"/dev/tcp/127.0.0.1/$PORT" # 워커가 잡고 요청을 기다림
sleep 0.2
exec 4<>"/dev/tcp/127.0.0.1/$PORT" # 큐에 대기
sleep 0.2
check "큐 포화 → 503" 503 "$(status_of /)"
check_contains "503 Retry-After" "Retry-After: 1" "$(curl -s -i "$(url /)")"
exec 3>&- 4>&-
sleep 0.3
check "붙잡은 연결을 놓으면 다시 200" 200 "$(status_of /)"

# ---- 종료: SIGINT 후 정상 종료 메시지 ----
stop_server
check_contains "SIGINT → 정상 종료" "정상 종료" "$(cat "$SERVER_LOG")"

finish
