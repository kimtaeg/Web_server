#!/usr/bin/env bash
# tools/gen-cert.sh — 로컬 테스트용 자체 서명(self-signed) 인증서와 개인키를 만든다.
#
# 사용: bash tools/gen-cert.sh [출력 디렉터리=certs] [만료 일수=365]
#   → <dir>/server.key (RSA 2048 개인키), <dir>/server.crt (X.509 인증서)
#
# - SAN(subjectAltName)에 localhost, 127.0.0.1, ::1 을 넣는다. 요즘 클라이언트는 CN 이 아니라
#   SAN 으로 호스트 이름을 검증한다.
# - 자체 서명이라 신뢰할 수 있는 CA 가 서명한 것이 아니다. curl 은 기본적으로 거부한다(exit 60).
#   테스트에서는 curl -k(검증 생략) 또는 curl --cacert <dir>/server.crt(이 인증서를 신뢰)로 접속한다.
# - 개인키는 비밀이다. 저장소에 커밋하지 않는다 (.gitignore 의 certs/).
set -euo pipefail
DIR=${1:-certs}
DAYS=${2:-365}
mkdir -p "$DIR"
# -nodes: 개인키를 암호로 잠그지 않는다 (서버가 시작할 때 암호를 물을 수 없으므로)
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
    -keyout "$DIR/server.key" -out "$DIR/server.crt" \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1" \
    -addext "basicConstraints=critical,CA:FALSE" 2>/dev/null
chmod 600 "$DIR/server.key"
echo "생성: $DIR/server.crt, $DIR/server.key"
openssl x509 -in "$DIR/server.crt" -noout -subject -enddate
