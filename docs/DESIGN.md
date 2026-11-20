# 설계 문서

각 결정은 "무엇을 / 왜 / 대안과 버린 이유" 순서로 적는다. 단계가 진행될 때마다 추가한다.

## 0. 전체 구조 (예정)

```
main ─ server(accept) ─▶ threadpool(bounded queue) ─▶ worker: conn_handle(fd)
                                                        │ keep-alive 루프
                                                        ▼
                               http_parser(증분, I/O 없음) ─▶ router
                                                        ├─ static
                                                        ├─ cgi
                                                        ├─ rproxy ─┐
                                                        └─ fproxy ─┴─ upstream (+ cache)
```

## 1. 빌드와 이식성

### OS 차이는 `src/platform.h` 한 곳에만
- **무엇**: `#ifdef __linux__ / __APPLE__`는 이 파일에만 쓴다. 다른 모듈은 `PLATFORM_SEND_FLAGS`,
  `platform_sock_init()` 같은 추상화만 호출한다.
- **왜**: 분기가 여러 파일에 흩어지면 한쪽 OS에서만 깨지는 버그를 찾기 어렵다. 파일 하나만 보면
  두 OS의 차이를 전부 설명할 수 있다.
- `static inline`만 쓰고 `.c`를 두지 않는다. 호출 비용이 없고, "차이 = 헤더 하나"가 유지된다.

### SIGPIPE를 두 겹으로 막기
- 끊긴 소켓에 `send`하면 SIGPIPE가 오고, 기본 동작은 **프로세스 종료**다. 클라이언트 하나가
  연결을 끊는 바람에 서버 전체가 죽으면 안 된다.
- 1겹: main에서 `SIGPIPE`를 `SIG_IGN`으로 설정 (2단계).
- 2겹: 소켓 단위. Linux는 `send(..., MSG_NOSIGNAL)`, macOS는 `MSG_NOSIGNAL`이 없어서
  `setsockopt(SO_NOSIGPIPE)`를 쓴다.
  1겹만 있어도 동작은 하지만, 라이브러리나 테스트가 시그널 설정을 바꿔도 안전하도록 2겹을 둔다.
- 검증: `tests/unit/test_platform.c`는 SIGPIPE를 일부러 기본값으로 두고 닫힌 소켓에 send해서
  EPIPE가 오는지 확인한다. SO_NOSIGPIPE 설정을 빼면 테스트 프로세스가 시그널 13으로 죽는 것도 확인했다.

### 빌드 모드 분리
- `build/{release,debug,asan}/`를 따로 쓴다. 플래그가 다른 `.o`를 섞어 링크하면
  ASan 런타임 불일치 같은 원인 모를 오류가 나기 때문이다.
- `-std=c11 -pedantic`에 `-D_POSIX_C_SOURCE=200809L`을 더하고, OS 확장이 필요한 부분만
  `_DARWIN_C_SOURCE` / `_DEFAULT_SOURCE`로 연다.
- macOS(arm64)의 ASan은 LeakSanitizer를 지원하지 않는다. 누수 검사는 Linux의 `make asan`에서
  하거나, macOS에서는 `leaks --atExit`로 한다.

## 2. 버퍼 (`buf`)

- **상한(max)을 버퍼가 직접 가진다.** 헤더 16KB 초과 → 431, 본문 초과 → 413처럼 "얼마나 쌓였나"가
  곧 오류 정책이기 때문이다. `BUF_ERR_LIMIT`(→ 4xx)와 `BUF_ERR_NOMEM`(→ 500)을 구분해서 돌려준다.
- **실패하면 내용이 바뀌지 않는다.** 일부만 쓰인 상태가 없으므로 호출자의 오류 처리가 단순해진다.
- **용량은 2배씩 늘린다.** append의 분할 상환 비용이 O(1)이 된다. 단, max+1보다 크게 잡지는 않는다.
- **consume은 memmove로 처리한다.** 읽기 오프셋을 두는 링버퍼보다 단순하다. 소비하고 남는 양은
  보통 파이프라이닝된 다음 요청 일부라서 복사 비용이 작다.
- **`buf_find(from)`**: 조각으로 도착할 때마다 처음부터 `\r\n\r\n`을 찾으면 O(n²)이 된다.
  이전에 검사한 길이 − 3부터 다시 찾으면 경계에 걸친 경우도 놓치지 않는다.
  `memmem`은 POSIX가 아니라서 `memchr` + `memcmp`로 직접 구현했다.
- 항상 NUL로 끝나게 유지한다(디버깅·로그용). 데이터는 바이너리여도 괜찮다.

## 3. 테스트
- 외부 라이브러리 없이 `tests/unit/test.h`의 매크로(`CHECK`, `REQUIRE`, `CHECK_EQ_*`)를 쓴다.
- 테스트 파일 하나 = 실행 파일 하나. 크래시나 시그널이 다른 테스트에 번지지 않는다.
  `tests/run_unit.sh`가 종료 코드로 성공/실패를 모은다.
