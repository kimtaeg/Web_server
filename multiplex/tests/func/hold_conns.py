#!/usr/bin/env python3
"""hold_conns.py <port> <n> [partial] — 연결 n 개를 열어 붙잡아 둔다 (기능 테스트 도우미).

- partial 을 주면 각 연결에 헤더 일부("GET / HTTP/1.1\\r\\nHost: a\\r\\n")를 보내 '요청 중' 상태로 만든다.
- 열린 개수를 "opened <k> refused <r>" 한 줄로 출력한 뒤, stdin 이 닫히거나 SIGTERM 을 받을 때까지 기다린다.
- 서버가 먼저 닫은(503 등) 연결 수는 끝날 때 "closed_by_server <c>" 로 출력한다.
bash 의 /dev/tcp 로는 fd 번호가 제한돼 수백 개를 다루기 어려워 파이썬으로 둔다 (표준 라이브러리만 사용).
"""
import resource
import select
import signal
import socket
import sys


def main():
    port = int(sys.argv[1])
    n = int(sys.argv[2])
    partial = len(sys.argv) > 3 and sys.argv[3] == "partial"

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = n + 64
    if soft != resource.RLIM_INFINITY and soft < want:
        target = want if hard == resource.RLIM_INFINITY else min(want, hard)
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
        except (ValueError, OSError):
            pass

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    socks, refused = [], 0
    for _ in range(n):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            if partial:
                s.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n")
            socks.append(s)
        except OSError:
            refused += 1
    print(f"opened {len(socks)} refused {refused}", flush=True)

    try:
        sys.stdin.read()  # 테스트 스크립트가 파이프를 닫으면 끝
    finally:
        closed = 0
        for s in socks:
            r, _, _ = select.select([s], [], [], 0)
            if r:  # 읽을 게 있다 = 서버가 응답(503 등)을 보냈거나 닫았다
                closed += 1
            s.close()
        print(f"closed_by_server {closed}", flush=True)


if __name__ == "__main__":
    main()
