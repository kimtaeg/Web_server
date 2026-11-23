#!/usr/bin/env python3
"""Raw-socket edge cases that curl cannot produce: malformed requests, limits, timeouts,
pipelining and abrupt disconnects. Usage: edge_cases.py HOST PORT"""
import socket
import struct
import sys
import time

HOST, PORT = sys.argv[1], int(sys.argv[2])
passed = failed = 0


def report(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  \033[32mok\033[0m   {name}")
    else:
        failed += 1
        print(f"  \033[31mFAIL\033[0m {name}  {detail}")


class Conn:
    """A client socket plus whatever was received beyond the last parsed response."""

    def __init__(self, timeout=5):
        self.sock = socket.create_connection((HOST, PORT), timeout=timeout)
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError
        self.buf += chunk

    def response(self, timeout=5):
        """One response (head + Content-Length body) -> (status, headers, body)."""
        self.sock.settimeout(timeout)
        try:
            while b"\r\n\r\n" not in self.buf:
                self._fill()
        except (EOFError, ConnectionResetError, socket.timeout):
            return None, {}, b""
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
        n = int(headers.get("content-length", "0"))
        try:
            while len(self.buf) < n:
                self._fill()
        except (EOFError, ConnectionResetError, socket.timeout):
            pass
        body, self.buf = self.buf[:n], self.buf[n:]
        return int(lines[0].split()[1]), headers, body

    def closed_by_peer(self, timeout=3):
        self.sock.settimeout(timeout)
        try:
            return self.buf == b"" and self.sock.recv(1) == b""
        except (ConnectionResetError, socket.timeout):
            return False

    def close(self, reset=False):
        if reset:  # SO_LINGER with zero timeout: close() sends RST instead of FIN
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.sock.close()


def status_of(raw, timeout=5):
    c = Conn()
    try:
        c.send(raw)
        return c.response(timeout)[0]
    except (ConnectionResetError, BrokenPipeError):
        return "reset"
    finally:
        c.close()


def expect(name, raw, want):
    got = status_of(raw)
    report(name, got == want, f"(expected {want}, got {got})")


OK = b"GET / HTTP/1.1\r\nHost: t\r\n\r\n"

print("-- malformed requests")
expect("HTTP/1.1 without Host -> 400", b"GET / HTTP/1.1\r\n\r\n", 400)
expect("two Host headers -> 400", b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400)
expect("HTTP/2.0 on the wire -> 505", b"GET / HTTP/2.0\r\nHost: t\r\n\r\n", 505)
expect("garbage version -> 400", b"GET / HTTX/1.1\r\nHost: t\r\n\r\n", 400)
expect("HTTP/0.9 style line -> 400", b"GET /\r\n\r\n", 400)
expect("unknown method -> 501", b"BREW /pot HTTP/1.1\r\nHost: t\r\n\r\n", 501)
expect("method with bad chars -> 400", b"G(T / HTTP/1.1\r\nHost: t\r\n\r\n", 400)
expect("space before colon -> 400", b"GET / HTTP/1.1\r\nHost : t\r\n\r\n", 400)
expect("obsolete line folding -> 400", b"GET / HTTP/1.1\r\nHost: t\r\nX: a\r\n b\r\n\r\n", 400)
expect("header without colon -> 400", b"GET / HTTP/1.1\r\nHost: t\r\nbroken\r\n\r\n", 400)
expect("bare CR in header -> 400", b"GET / HTTP/1.1\r\nHost: t\rX: y\r\n\r\n", 400)
expect("NUL byte in head -> 400", b"GET / HTTP/1.1\r\nHost: t\x00\r\n\r\n", 400)
expect("%00 in path -> 400", b"GET /a%00b HTTP/1.1\r\nHost: t\r\n\r\n", 400)
expect("bad percent escape -> 400", b"GET /a%zz HTTP/1.1\r\nHost: t\r\n\r\n", 400)
expect("relative target -> 400", b"GET index.html HTTP/1.1\r\nHost: t\r\n\r\n", 400)
expect("Content-Length not a number -> 400",
       b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: abc\r\n\r\n", 400)
expect("conflicting Content-Length -> 400",
       b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab", 400)
expect("Content-Length + Transfer-Encoding -> 400 (smuggling)",
       b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400)
expect("unsupported Transfer-Encoding -> 501",
       b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip\r\n\r\n", 501)
expect("malformed chunk size -> 400",
       b"POST /cgi-bin/form.sh HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 400)
expect("Expect: something-else -> 417",
       b"POST / HTTP/1.1\r\nHost: t\r\nExpect: tea\r\nContent-Length: 1\r\n\r\nx", 417)
expect("leading empty lines are tolerated", b"\r\n\r\n" + OK, 200)

print("-- size limits")
expect("10 KB URI -> 414", b"GET /" + b"a" * 10000 + b" HTTP/1.1\r\nHost: t\r\n\r\n", 414)
expect("40 KB of headers -> 431",
       b"GET / HTTP/1.1\r\nHost: t\r\nX-Big: " + b"a" * 40000 + b"\r\n\r\n", 431)
expect("100 header fields -> 431",
       b"GET / HTTP/1.1\r\nHost: t\r\n" + b"".join(b"X-%d: v\r\n" % i for i in range(100)) + b"\r\n", 431)
expect("huge Content-Length -> 413 before the body is sent",
       b"POST /cgi-bin/form.sh HTTP/1.1\r\nHost: t\r\nContent-Length: 999999999999\r\n\r\n", 413)
expect("Content-Length overflow -> 413",
       b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 99999999999999999999999\r\n\r\n", 413)
expect("chunked body over the limit -> 413",
       b"POST /cgi-bin/form.sh HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFF\r\n", 413)

print("-- framing, keep-alive, pipelining")
c = Conn()
c.send(b"GET /css/style.css HTTP/1.1\r\nHost: t\r\n\r\nGET /js/app.js HTTP/1.1\r\nHost: t\r\n\r\n"
       b"GET /nope HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
r = [c.response() for _ in range(3)]
report("3 pipelined requests answered in order",
       [x[0] for x in r] == [200, 200, 404] and r[0][1].get("content-type", "").startswith("text/css")
       and r[1][1].get("content-type", "").startswith("text/javascript"), f"got {[x[0] for x in r]}")
report("Connection: close honoured after the pipeline", c.closed_by_peer())
c.close()

c = Conn()
c.send(b"GET / HTTP/1.0\r\n\r\n")
st = c.response()[0]
report("HTTP/1.0 without Host is fine, then closed", st == 200 and c.closed_by_peer())
c.close()

c = Conn()
statuses = []
for _ in range(3):
    c.send(OK)
    st, h, _ = c.response()
    statuses.append((st, h.get("connection")))
report("3 sequential requests on one keep-alive connection", statuses == [(200, "keep-alive")] * 3,
       str(statuses))
time.sleep(3.2)  # keepalive_timeout is 2 s in the test config
report("idle keep-alive connection closed after keepalive_timeout", c.closed_by_peer(1))
c.close()

c = Conn()
c.send(b"POST /cgi-bin/form.sh HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n")
interim = c.response(3)
c.send(b"hello")
final = c.response()
report("Expect: 100-continue -> interim 100, then 200",
       interim[0] == 100 and final[0] == 200 and b"hello" in final[2], f"{interim[0]} {final[0]}")
c.close()

c = Conn()
c.send(b"POST /cgi-bin/form.sh HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
       b"4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: 1\r\n\r\n")
st, _, body = c.response()
report("chunked body with extensions and trailers decoded", st == 200 and b"Wikipedia" in body)
c.close()

print("-- timeouts and abrupt disconnects")
c = Conn()
c.send(b"GET / HTTP/1.1\r\nHost: t\r\n")  # never finishes the head
t0 = time.time()
st = c.response(6)[0]
elapsed = time.time() - t0
report("incomplete head -> 408 after request_timeout", st == 408 and 1.5 < elapsed < 4,
       f"status {st} after {elapsed:.1f}s")
c.close()

c = Conn()
c.send(b"POST /cgi-bin/form.sh HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\nonly-part")
st = c.response(6)[0]
report("body shorter than Content-Length -> 408", st == 408, f"got {st}")
c.close()

for _ in range(20):
    c = Conn()
    c.send(b"GET /index.html HTTP/1.1\r\nHo")
    c.close()
c = Conn()
c.send(b"GET /__test/big.bin HTTP/1.1\r\nHost: t\r\n\r\n")
c.sock.recv(1000)
c.close(reset=True)  # RST in the middle of a 3 MB response
time.sleep(0.3)
expect("server healthy after 20 half-requests and a reset mid-download", OK, 200)

idle = [Conn() for _ in range(300)]
t0 = time.time()
st = status_of(OK)
elapsed = time.time() - t0
report("300 idle connections do not starve the 8 worker threads", st == 200 and elapsed < 1,
       f"status {st} in {elapsed:.2f}s")
for c in idle:
    c.close()

print(f"\nedge cases: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
