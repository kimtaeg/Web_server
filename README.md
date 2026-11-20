# cweb — C로 만든 HTTP/1.1 서버 + 리버스/포워드 프록시

외부 HTTP·이벤트 라이브러리 없이 **C11 + POSIX(libc, pthread)만으로** 만든 웹 서버입니다.
정적 파일, CGI, 리버스 프록시(경로·Host 라우팅 + 라운드로빈), 포워드 프록시(+ LRU 캐시)를 지원하고,
macOS와 Linux에서 같은 코드로 빌드됩니다.

설계의 "왜"는 [`docs/DESIGN.md`](docs/DESIGN.md), 부하 테스트 전체 결과는 [`docs/benchmark.md`](docs/benchmark.md)에 있습니다.

## 기능

| 영역 | 내용 |
|---|---|
| HTTP/1.1 | I/O 없는 증분 파서(1바이트씩 넣어도 결과가 같음을 테스트로 증명), keep-alive, 파이프라이닝, HEAD |
| 오류 정책 | 400 / 403 / 404 / 405(+Allow) / 408 / 411 / 413 / 414 / 431 / 500 / 501 / 502 / 503 / 504 / 505 / 508 |
| 정적 파일 | 퍼센트 디코딩 → 정규화 → realpath 경계 검사(루트 탈출 403), 디렉터리 301/index.html, MIME |
| CGI | fork + 파이프 2개 + execve, RFC 3875 환경 변수, 타임아웃 시 프로세스 그룹 kill → 504, 좀비 없음 |
| 리버스 프록시 | 최장 접두사·Host 규칙, 원자적 라운드로빈, 연결 실패 시 down 표시 + 1회 재시도, 응답 스트리밍 |
| 포워드 프록시 | absolute-form(`curl -x`), getaddrinfo, LRU 캐시(해시맵 + 이중 연결 리스트 + rwlock), `X-Cache` |
| 동시성 | main 스레드 accept → bounded queue → 고정 워커 풀, 큐가 차면 503 |
| 견고성 | graceful shutdown, slowloris 헤더 마감(408), SIGPIPE 방어, CLOEXEC, 접근 로그 |

## 빌드와 실행

```sh
make                 # build/release/webserver + 예제 CGI(build/release/cgi-bin/)
make debug           # -O0 -g3
make test            # 단위 테스트 (17개 파일)
make asan            # ASan + UBSan 으로 단위 테스트
make func            # 기능 테스트: 실제 서버를 띄우고 curl/nc 로 검증 (tests/func/*.sh)
make func-asan       # 기능 테스트 전체를 ASan 서버로
make leakcheck       # 서버 실행 → 요청 → SIGINT → 누수 0 (Linux: LSan, macOS: leaks)
make valgrind        # (Linux) 단위 + 기능 테스트를 valgrind 로
bash tests/load/run.sh   # wrk 부하 테스트 → docs/benchmark.md
```

```sh
# 정적 파일 + CGI
./build/release/webserver -p 8080 -r ./examples/www -g ./build/release/cgi-bin
curl 'http://localhost:8080/cgi-bin/adder?a=3&b=4'        # 7

# 설정 파일로 (명령줄 옵션이 설정 파일보다 우선)
./build/release/webserver -f examples/cweb.conf
```

주요 옵션: `-p` 포트, `-r` 문서 루트, `-w` 워커 수, `-q` 큐 길이, `-t` 유휴 타임아웃(ms),
`-T` 헤더 마감(ms), `-g` CGI 디렉터리, `-c` CGI 타임아웃(ms), `-n/--name` 백엔드 이름(`X-Backend` 헤더), `-f` 설정 파일.

## 설정 예시

```conf
listen   0.0.0.0:8080
root     ./www
cgi      /cgi-bin ./build/release/cgi-bin
workers  16
queue    256
idle_timeout_ms   5000
header_timeout_ms 10000          # slowloris 방어: 요청 첫 바이트 → 헤더 끝 총 시간

# 리버스 프록시: 최장 접두사 우선, 같은 길이면 Host 규칙 우선
route /api               127.0.0.1:9001 127.0.0.1:9002
route host=admin.local / 127.0.0.1:9100
upstream_connect_timeout_ms 1000
upstream_read_timeout_ms    5000
upstream_down_secs          10   # 연결 실패한 백엔드를 이 시간 동안 제외

# 포워드 프록시 (기본 off: open proxy 방지)
forward_proxy on
cache_max_bytes        67108864
cache_max_object_bytes 1048576
cache_default_ttl_secs 60
```

같은 바이너리로 로드밸런싱을 확인하는 법:
```sh
./build/release/webserver -p 9001 -r ./examples/www --name A &
./build/release/webserver -p 9002 -r ./examples/www --name B &
./build/release/webserver -p 8080 -f examples/cweb.conf &
for i in 1 2 3 4; do curl -s -D - -o /dev/null localhost:8080/api/ | grep X-Backend; done   # A B A B
curl -x localhost:8080 http://localhost:9001/ -D - -o /dev/null | grep X-Cache              # MISS → HIT
```

## 아키텍처

```
                 ┌──────────── main 스레드 ────────────┐
 클라이언트 ──TCP──▶ poll(500ms) → accept → try_submit ──┼──▶ bounded queue (mutex + cond 2개)
                 │        큐가 가득 차면 503 + close    │            │
                 └──────────────────────────────────────┘            ▼
                                                    워커 N개: conn_handle(fd)  — keep-alive 루프
                                                      recv(poll: 유휴 타임아웃 + 헤더 마감)
                                                      → http_parser (증분, I/O 없음, malloc 0)
                                                      → router (최장 접두사 / Host / absolute-form)
                                       ┌─────────────┬──────────┼─────────────┬─────────────────┐
                                       ▼             ▼          ▼             ▼                 │
                                    static         cgi       rproxy        fproxy ── cache(LRU) │
                                 (realpath 검사) (fork/exec)  (RR, down)   (getaddrinfo)          │
                                                               └──── upstream ────┘             │
                                                     (non-blocking connect, 재작성, 스트리밍 중계)  │
 종료: SIGINT → 플래그 → 리슨 소켓 close → 큐 close(워커 깨움) → 진행 중 요청 완료 → join → free
```

모듈: `main, server, threadpool, conn, buf, netio, http_parser, http_response, mime, static, cgi, router,
upstream, rproxy, fproxy, cache, config, log, platform.h` — OS 차이는 `platform.h`에만 있습니다.

## 테스트

| 종류 | 내용 |
|---|---|
| 단위 (17개 파일) | 파서 분할 무관성(1바이트씩, 무작위 분할, 변형 입력 2000개), 오류 테이블, 경로 탈출, LRU 순서와 동시 접근, RR 원자성, 요청 재작성 등 |
| 기능 (`tests/func`) | static 34, cgi 42, rproxy 26, fproxy 36, abort 8, edge 31 — 실제 서버 + curl/nc (각 스크립트 마지막에 서버 로그의 sanitizer 오류 검사 포함) |
| 메모리 | ASan + UBSan(단위·기능 전체), leaks/LSan, valgrind(CI) |
| 테스트를 테스트 | 주요 방어 코드를 일부러 깨면(변이) 해당 테스트가 실패하는 것을 확인 (DESIGN.md 각 절) |
| CI | GitHub Actions: ubuntu-latest, macos-latest (+ valgrind) |

## 부하 테스트

Apple M2 / macOS 26.3, 루프백, wrk 4.2.0, 1KB 정적 파일, keep-alive, 조합당 8초 1회 측정.
전체 표와 측정 조건은 [`docs/benchmark.md`](docs/benchmark.md)에 있습니다(`bash tests/load/run.sh`로 재생성).

| 대상 | 워커 | 연결 10 (req/s, p99) | 연결 100 (req/s, p99) | 연결 500 (req/s, 2초 초과) |
|---|---:|---|---|---|
| 정적 | 4 | 6,766 / 85ms | 6,528 / 1.70s | 6,471 / 388건 |
| 정적 | 8 | 7,806 / 77ms | 8,528 / 1.24s | 9,917 / 594건 |
| 정적 | 16 | 12,321 / 34ms | 10,603 / 895ms | 10,853 / 629건 |
| 리버스 프록시 | 4 | 4,035 / 141ms | 5,048 / 1.87s | 3,626 / 211건 |
| 리버스 프록시 | 8 | 5,687 / 51ms | 6,432 / 1.42s | 3,521 / 185건 |
| 리버스 프록시 | 16 | 1,789 / 271ms | 6,324 / 1.64s | 7,114 / 437건 |

읽는 법:
- **처리량은 워커 수에 따라 늘어납니다**(정적, 연결 10: 워커 4 → 16에서 6.8k → 12.3k req/s).
- **연결 수가 워커 수를 넘으면 지연이 급증합니다.** 연결 100에서 p99가 1초를 넘고, 연결 500에서는 2초 안에
  응답을 못 받는 요청이 수백 건 나옵니다. 워커가 keep-alive 연결을 붙잡고 있는 동안 나머지 연결은 큐에서
  기다리기 때문입니다(아래 "한계").
- **리버스 프록시는 정적 파일보다 느립니다.** 한 단계를 더 거치고, 업스트림 연결을 요청마다 새로 맺기 때문입니다.
- 오류 응답(non-2xx)은 모든 조합에서 0입니다.
- 1회 측정이라 편차가 있습니다. 예를 들어 리버스 프록시 워커 16, 연결 10의 1,789 req/s는 다른 조합과
  맞지 않는 값인데, 원인을 확인하지 못했습니다. 연결 하나로 잴 때 중앙값은 0.15ms인데 p99가 20~50ms로 튀는
  현상도 원인을 찾지 못했습니다(서버 쪽 Nagle, 접근 로그, keep-alive는 원인이 아님을 확인).

## 한계 (C10K)

- **동시에 처리할 수 있는 연결 수 = 워커 수.** 연결 하나를 워커 하나가 keep-alive 동안 붙잡습니다.
  위 표에서 `-c 500`일 때 워커 수를 넘는 연결은 큐에서 기다리다 타임아웃됩니다. 연결 수만 개(C10K)는
  이 구조로 감당할 수 없습니다.
- **slowloris 방어는 부분적입니다.** 헤더 마감은 한 연결이 워커를 붙잡는 시간을 최대 10초로 제한할 뿐,
  워커 수만큼 연결을 동시에 열어 두는 공격은 막지 못합니다(`edge.sh`의 slowloris x4).
- 업스트림 커넥션 풀이 없습니다(요청마다 연결). 리버스 프록시 처리량이 정적 파일보다 낮은 이유 중 하나이고,
  TIME_WAIT 소켓이 쌓여 임시 포트가 고갈될 수 있습니다. 그때는 백엔드를 down으로 표시하지 않고 503을 보냅니다.
- chunked 요청 본문(411), https와 CONNECT(501), HTTP/2는 지원하지 않습니다.

## 다음 단계: epoll/kqueue 하이브리드

I/O 대기는 이벤트 루프(Linux epoll / macOS kqueue — `platform.h`로 감쌈)가 맡고, **완성된 요청만**
워커 풀에 넘기는 구조입니다. 유휴 연결과 느린 연결은 워커를 차지하지 않고 fd와 버퍼만 차지하게 됩니다.
파서가 이미 I/O 없는 증분 파서라서, 이벤트 루프는 받은 바이트를 쌓다가 `http_parse()`가 OK를 줄 때만
넘기면 됩니다. 이어서 업스트림 커넥션 풀, 연결당 메모리 상한을 둘 계획입니다.
