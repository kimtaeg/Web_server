# cwebserv — C 로 처음부터 만든 HTTP/1.1 웹 서버 + 리버스 · 포워드 프록시

외부 HTTP · 이벤트 라이브러리 없이 **POSIX 소켓 API 와 pthread 만으로** 작성한 웹 서버입니다.
정적 파일, CGI, 리버스 프록시(라운드 로빈 · 장애 조치), 포워드 프록시(LRU 캐시 · CONNECT 터널)를 한 프로세스에서 처리합니다.

- 코드: `src/` 약 4,200줄 (C11, `-Wall -Wextra -Wpedantic -Wshadow` 경고 0)
- 테스트: 단위 1,423 검사 · 기능 79 · 엣지 케이스 39 — ASan/UBSan 빌드에서도 모두 통과, `leaks` 0 바이트
- 플랫폼: macOS(arm64) · Linux. 다른 곳은 `#ifdef` 몇 군데뿐입니다(`sendfile`, `SIGPIPE`, `getopt`).

**구성도**: [`docs/웹서버_구성도.html`](docs/웹서버_구성도.html) — 브라우저로 여는 단일 파일. 90개 개념 · 17장의 구성도에서 칸을 누르면 해설과 **실제 소스 코드**(파일 · 줄 번호)가 뜨고, 하위 구성도로 내려갈 수 있습니다.

## 빠른 시작

```bash
make                     # build/webserv, build/echo_backend
make demo                # 백엔드 3개 + conf/webserv.conf 로 서버 실행 (Ctrl-C 로 종료)

curl -i http://127.0.0.1:8080/                          # 정적 파일
curl    http://127.0.0.1:8080/cgi-bin/hello.sh?name=c   # CGI
curl    http://127.0.0.1:8080/api/hello                 # 리버스 프록시 → 9001 / 9002 번갈아
curl -H 'Host: app.localhost' http://127.0.0.1:8080/    # Host 기반 라우팅 → 9003
curl -x http://127.0.0.1:8080 http://127.0.0.1:9001/cacheable -i   # 포워드 프록시 (X-Cache: MISS → HIT)
curl    http://127.0.0.1:8080/__status                  # 통계 JSON
```

설정 파일 없이 CLI 만으로도 됩니다.

```bash
./build/webserv -p 8080 -r www -t 8 --cgi /cgi-bin=cgi-bin \
  -u api=127.0.0.1:9001,127.0.0.1:9002 --route-strip /api=api \
  --forward-proxy --cache -s 'proxy_timeout 2000'
```

`-c FILE` 이 먼저 적용되고 나머지 CLI 옵션이 그 위에 덮입니다. 모든 지시어는 [`conf/webserv.conf`](conf/webserv.conf) 에 주석과 함께 있고, `-s 'directive args'` 로 CLI 에서도 줄 수 있습니다. 잘못된 값은 `파일:줄: 이유` 로 알려 주고 종료 코드 2 로 끝납니다.

## 구조

```
 client ──TCP──▶ listener ──새 fd──▶ poll 리액터 ──읽을 것 있음──▶ 스레드 풀 워커
                                       ▲                              │ HTTP 파서
                                       └──── keep-alive 연결 반납 ─────┤ 디스패치
                                                                      ▼
                         CONNECT 터널 · 포워드 프록시(+캐시) · /__status · 리버스 프록시 · CGI · 정적 파일
```

| 계층 | 파일 | 하는 일 |
|---|---|---|
| 소켓 | `netutil.c` | `socket → SO_REUSEADDR → bind → listen`, `TCP_NODELAY`, 논블로킹 connect + 타임아웃 |
| 리액터 | `server.c` | 메인 스레드의 `poll()` 루프: accept, 유휴 keep-alive 연결 보관, self-pipe 로 깨우기, 유휴 정리 |
| 워커 | `threadpool.c`, `server.c` | 고정 크기 스레드 풀. 연결 하나를 받아 요청을 읽고(마감 있음) 처리한 뒤 리액터에 반납 |
| HTTP | `http.c` | 소켓을 모르는 순수 파서: 요청 줄 · 헤더 · Content-Length / chunked 본문, URI 정규화 |
| 분기 | `handler.c`, `router.c` | 처리기 선택, 경로 접두사 · Host 라우팅, 라운드 로빈 + 수동 헬스 체크 |
| 처리기 | `static_file.c`, `cgi.c`, `proxy.c`, `cache.c` | 정적 파일(sendfile · ETag), CGI(fork/exec), 프록시 중계, LRU 캐시 |
| 응답 | `response.c` | 상태 줄 · 표준 헤더 · 오류 페이지, keep-alive 판단 |

### 설계 판단

- **동시성 = 리액터 + 스레드 풀.** 쉬는 연결은 `poll()` 집합에만 있고 스레드를 쓰지 않습니다. 읽을 데이터가 생긴 연결만 워커가 블로킹 코드로 처리합니다. 그래서 워커 8개로 keep-alive 연결 1,000개를 동시에 받아도 74,824 req/s, 실패 0 이었고, 아무것도 보내지 않는 연결 300개가 열려 있어도 새 요청이 막히지 않습니다. `fork` 모델(격리는 좋지만 비쌈 — 이 서버의 CGI 가 그 비용을 보여 줌), 순수 이벤트 루프(빠르지만 모든 I/O 를 상태 기계로)와의 비교는 구성도의 *설계 판단* 에 있습니다.
- **`poll()` 을 고른 이유**: POSIX 표준이라 macOS · Linux 에서 같은 코드입니다. 연결 1만 개 이상에서는 O(n) 비용이 보이므로 `epoll`/`kqueue` 백엔드가 다음 단계입니다.
- **파서는 순수 함수.** 같은 버퍼를 다시 넣으면 같은 결과이므로, 요청을 한 바이트씩 잘라 넣는 테스트로 경계 처리를 검증했습니다.
- **프록시 → 백엔드는 요청당 연결 하나(`Connection: close`).** 응답 경계가 단순하고 실패가 다음 요청을 오염시키지 않습니다. 대가는 TCP 연결 비용입니다(업스트림 연결 풀이 개선 후보).
- **재시도는 아무것도 보내지 않았을 때만.** 연결 실패면 다음 백엔드로 넘어가지만, 요청을 보낸 뒤의 실패는 재시도하지 않습니다 — POST 가 두 번 실행되지 않도록.

## 엣지 케이스와 보안

| 상황 | 동작 |
|---|---|
| 깨진 요청 줄 · 헤더, `Host` 없음/중복, 접힌 헤더, NUL, 잘못된 chunk | 400 |
| 모르는 메서드 / 전송 코딩 · HTTP 버전 | 501 / 505 |
| URI 8 KB 초과 · 요청 헤드(요청 줄 + 헤더) 24 KB 또는 헤더 64개 초과 · 본문 `max_body_size` 초과 | 414 · 431 · 413 (본문을 받기 **전에**) |
| `Content-Length` + `Transfer-Encoding`, 서로 다른 `Content-Length` 둘 (요청 밀반입) | 400 |
| `../`, `%2e%2e`, `%00`, 루트 밖을 가리키는 심볼릭 링크, `.env` 같은 숨김 파일 | 403 (realpath 로 세그먼트 경계까지 비교) |
| 헤더를 반쯤 보내고 멈춤(slowloris), 본문이 모자람 | `request_timeout` 뒤 408 |
| 백엔드 연결 거부 · 엉터리 응답 / 응답 없음 | 다음 백엔드로, 모두 실패면 502 / 504 |
| 프록시 고리 (`Via` 에 자기 표시) | 508 |
| 다운로드 중 클라이언트가 끊음 | `SIGPIPE` 무시 + `MSG_NOSIGNAL`/`SO_NOSIGPIPE`, 그 연결만 정리 |
| fd 고갈(EMFILE) | accept 를 잠시 멈추고 기존 연결은 계속 처리 |
| CGI 가 멈춤 | `cgi_timeout` 뒤 **프로세스 그룹 전체** SIGKILL, 504 |
| SIGINT / SIGTERM | 리스너 닫기 → 진행 중 요청 마무리 → 워커 join → 모든 메모리 해제, 종료 코드 0 |

포워드 프록시는 아무 곳에나 연결해 주므로 기본은 꺼져 있고, 샘플 설정은 `bind 127.0.0.1` 입니다. CONNECT 는 443 만 허용합니다. CGI 에는 `HTTP_PROXY` 를 넘기지 않습니다(httpoxy).

## 테스트

```bash
make test        # 단위 테스트 + 기능 테스트(curl) + 엣지 케이스(raw socket)
make asan        # ASan + UBSan 빌드 → build-asan/
./build-asan/unit_tests && ./tests/functional.sh build-asan
make memcheck    # macOS: leaks --atExit / Linux: valgrind — 연결을 열어 둔 채 SIGTERM
make load        # ApacheBench 부하 측정 → 표 출력
```

- **단위** (`tests/unit/`, 프레임워크 없음): 파서 오류 30여 종의 정확한 상태 코드, 한 바이트씩 넣기, chunked 스캐너, URI 정규화, MIME, 라우팅 · 라운드 로빈 분배(30회 → 10/10/10), LRU 퇴출 · TTL, 설정 파싱, 경로 탈출, 프록시 헤더 재작성, CGI 출력 해석, 스레드 풀.
- **기능** (`tests/functional.sh`): 테스트용 백엔드(`tools/echo_backend.c`)를 띄우고 curl 로 정적 · CGI · 라운드 로빈 · 장애 조치(백엔드 하나 죽여도 6/6 성공) · 502/504 · 캐시 MISS→HIT · CONNECT · keep-alive 재사용 · 동시 200 요청 · 종료를 확인합니다.
- **엣지** (`tests/edge_cases.py`): curl 이 만들 수 없는 요청을 날 소켓으로 — 위 표의 상황들, 파이프라이닝 순서, 100-continue, 3 MB 수신 중 RST.

## 성능

macOS · Apple Silicon 10코어 · 루프백, 워커 8개, keep-alive ([`docs/benchmark.md`](docs/benchmark.md) 에 워커 1개 결과까지 전체 표).

| 시나리오 | 동시 | req/s | p99 |
|---|---:|---:|---:|
| 정적 2 KB | 50 | 81,243 | 1 ms |
| 정적 2 KB, 요청마다 새 연결 | 50 | 46,719 | 2 ms |
| 정적 3 MB (`sendfile`, ≈ 13 GB/s) | 10 | 4,379 | 4 ms |
| 리버스 프록시 → 백엔드 2 | 50 | 32,139 | 2 ms |
| 포워드 프록시 캐시 HIT | 50 | 155,128 | 1 ms |
| CGI (요청마다 fork + exec) | 10 | 1,417 | 27 ms |
| 정적, keep-alive 연결 1,000개 | 1,000 | 74,824 | 22 ms |

모든 시나리오에서 연결 · 수신 오류 0. (ab 는 응답 길이가 달라지면 "Failed" 로 세므로, 길이가 바뀌는 백엔드 응답은 `tests/load.sh` 가 따로 집계합니다.)

## 디렉터리

```
src/            서버 (main.c 와 모듈별 .c/.h)
tests/          unit/ · functional.sh · edge_cases.py · load.sh · memcheck.sh · demo.sh
tools/          echo_backend.c — 테스트 · 데모용 백엔드
conf/           webserv.conf — 주석 달린 샘플 설정
www/ cgi-bin/   정적 파일 루트와 CGI 스크립트 예제
docs/           웹서버_구성도.html (구성도), benchmark.md
docs/웹서버_구성도_원본/   구성도 데이터 (개념 · 구성도 배치)
docs/atlas/     구성도 빌드: build.mjs · check_refs.mjs · engine/
```

### 구성도 다시 만들기

```bash
node docs/atlas/build.mjs docs/웹서버_구성도_원본 -o docs/웹서버_구성도.html   # 링크 · 배치 검사 후 생성
node docs/atlas/check_refs.mjs docs/웹서버_구성도_원본 .                      # 인용한 코드가 소스와 같은지
```

`build.mjs` 는 깨진 링크, 트리에 없는 개념, 구성도에 빠진 하위 개념, 칸을 지나가는 선, 없는 아이콘이 하나라도 있으면 파일을 쓰지 않습니다. `check_refs.mjs` 는 구성도에 실린 코드 조각의 모든 줄이 소스에 그대로 있는지 확인합니다(현재 80개 위치 · 472줄 일치). 소스를 고친 뒤에는 둘 다 다시 돌려 줄 번호를 맞춥니다.
