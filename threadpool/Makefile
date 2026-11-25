# 사용법
#   make / make all  : 릴리스 바이너리   build/release/webserver
#   make debug       : 디버그 바이너리   build/debug/webserver   (-O0 -g3)
#   make test        : 단위 테스트 빌드 + 실행 (debug 설정)
#   make asan        : ASan + UBSan 으로 바이너리와 단위 테스트를 빌드하고 테스트 실행
#   make func        : 기능 테스트 (서버를 띄우고 curl 로 검증, tests/func/*.sh)
#   make leakcheck   : 서버 실행 → 요청 → SIGINT → 누수 0 확인
#   make func-asan   : 기능 테스트 전체를 ASan 빌드 서버로
#   make valgrind    : (Linux) 단위 + 기능 테스트를 valgrind 로, 오류·누수 0 확인
#   make cert        : HTTPS 테스트용 자체 서명 인증서 certs/server.{crt,key} 생성
#   make clean       : build/ 삭제
#
# 모드마다 build/<mode>/ 에 따로 빌드한다. 같은 .o 를 다른 플래그로 섞어 링크하는 사고를 막기 위해서다.
# GNU make 3.81(macOS 기본)에서도 동작하도록 신기능(.RECIPEPREFIX 등)은 쓰지 않는다.

UNAME_S := $(shell uname -s)

CSTD := -std=c11 -pedantic -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
# _POSIX_C_SOURCE 만 켜면 OS 확장(macOS 의 SO_NOSIGPIPE, sendfile 등)이 가려진다.
ifeq ($(UNAME_S),Darwin)
  CSTD += -D_DARWIN_C_SOURCE
  # macOS(arm64)의 ASan 은 LeakSanitizer 를 지원하지 않는다.
  SAN_ENV := UBSAN_OPTIONS=print_stacktrace=1
else
  CSTD += -D_DEFAULT_SOURCE
  SAN_ENV := ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1
endif

MODE ?= release
LDFLAGS :=
ifeq ($(MODE),release)
  MODEFLAGS := -O2 -g
else ifeq ($(MODE),debug)
  MODEFLAGS := -O0 -g3
else ifeq ($(MODE),asan)
  MODEFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
  LDFLAGS += -fsanitize=address,undefined
else
  $(error 알 수 없는 MODE=$(MODE) (release|debug|asan))
endif

# ---- OpenSSL (HTTPS) ----
# pkg-config 가 있으면 그것을, 없으면 macOS Homebrew(openssl@3, openssl@1.1) 경로를 쓴다.
# OPENSSL_PREFIX=/경로 로 직접 지정할 수도 있다. 1.1.0 이상 필요.
# -isystem: OpenSSL 헤더 안의 경고가 -Werror -pedantic 에 걸리지 않게 (우리 코드에는 그대로 적용).
# OPENSSL_API_COMPAT: 1.1.0 API 기준으로 컴파일 (그보다 오래된 함수를 쓰면 컴파일 오류).
OPENSSL_PREFIX ?= $(shell pkg-config --variable=prefix openssl 2>/dev/null)
ifeq ($(OPENSSL_PREFIX),)
  OPENSSL_PREFIX := $(firstword $(foreach v,openssl@3 openssl@1.1 openssl,$(shell brew --prefix $(v) 2>/dev/null)))
endif
# /usr 이면 기본 검색 경로라 아무것도 붙이지 않는다 (-isystem /usr/include 는 include 순서를 깬다)
ifneq ($(filter-out /usr,$(OPENSSL_PREFIX)),)
  SSL_CFLAGS := -isystem $(OPENSSL_PREFIX)/include
  SSL_LIBS := -L$(OPENSSL_PREFIX)/lib
endif
SSL_CFLAGS += -DOPENSSL_API_COMPAT=0x10100000L
SSL_LIBS += -lssl -lcrypto

CFLAGS := $(CSTD) $(MODEFLAGS) -Isrc $(SSL_CFLAGS) -MMD -MP
LDLIBS := -pthread $(SSL_LIBS)

# 구현이 끝난 모듈만 나열한다. 빈 모듈은 -pedantic 에서 "빈 번역 단위" 오류가 나므로
# 각 단계에서 구현할 때 여기에 추가한다.
MODULES := buf http_parser mime netio log http_response threadpool server conn static router cgi config upstream rproxy cache fproxy tls connio

BUILD     := build/$(MODE)
LIB_OBJS  := $(patsubst %,$(BUILD)/obj/%.o,$(MODULES))
MAIN_OBJ  := $(BUILD)/obj/main.o
BIN       := $(BUILD)/webserver
TEST_SRCS := $(wildcard tests/unit/test_*.c)
TEST_BINS := $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(TEST_SRCS))
# 예제 CGI 프로그램: cgi-bin/*.c → build/<mode>/cgi-bin/<이름> (문서 루트 밖)
CGI_SRCS  := $(wildcard cgi-bin/*.c)
CGI_BINS  := $(patsubst cgi-bin/%.c,$(BUILD)/cgi-bin/%,$(CGI_SRCS))

.PHONY: all debug asan test func func-asan valgrind leakcheck clean bin unit unit-run cert

all: bin

debug:
	@$(MAKE) --no-print-directory MODE=debug bin

test:
	@$(MAKE) --no-print-directory MODE=debug unit-run

asan:
	@$(MAKE) --no-print-directory MODE=asan bin unit-run SAN=1

# 기능 테스트 전체를 ASan+UBSan 빌드 서버로 (서버 로그의 sanitizer 오류는 lib.sh 가 실패로 잡는다)
func-asan:
	@$(MAKE) --no-print-directory MODE=asan bin
	@set -e; for t in tests/func/*.sh; do case $$t in */lib.sh) continue;; esac; \
		$(SAN_ENV) bash $$t build/asan/webserver; done

# valgrind (Linux 전용): 단위 테스트 + 기능 테스트 서버를 valgrind 아래에서. 판정은 메모리 오류·누수만.
valgrind:
	@$(MAKE) --no-print-directory MODE=debug bin unit
	@$(MAKE) --no-print-directory MODE=release bin
	@bash tests/valgrind/run.sh

# 누수 검사: 서버 실행 → 요청 → SIGINT → 누수 보고 (Linux: LSan, macOS: leaks + ASan)
leakcheck:
	@$(MAKE) --no-print-directory MODE=asan bin
	@$(MAKE) --no-print-directory MODE=debug bin
	@$(MAKE) --no-print-directory MODE=release bin
	@bash tests/leak/leakcheck.sh build/asan/webserver build/debug/webserver

# 기능 테스트: 실제 서버를 띄우고 curl 로 검증 (포트와 curl 이 필요해서 test 와 분리)
func: bin
	@set -e; for t in tests/func/*.sh; do case $$t in */lib.sh) continue;; esac; bash $$t $(BIN); done

bin: $(BIN) $(CGI_BINS)

unit: $(TEST_BINS)

unit-run: unit
	@$(if $(SAN),$(SAN_ENV)) sh tests/run_unit.sh $(TEST_BINS)

$(BIN): $(MAIN_OBJ) $(LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# 테스트 하나 = 실행 파일 하나. 테스트 소스 + 구현된 모듈 오브젝트(main.o 제외)를 링크한다.
$(BUILD)/tests/%: tests/unit/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/unit $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS)

$(BUILD)/cgi-bin/%: cgi-bin/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# 로컬 실행용 자체 서명 인증서 (테스트 스크립트는 각자 임시 인증서를 만든다)
cert:
	@bash tools/gen-cert.sh certs

clean:
	rm -rf build

-include $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_BINS:=.d) $(CGI_BINS:=.d)
