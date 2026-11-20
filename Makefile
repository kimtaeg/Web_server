# 사용법
#   make / make all  : 릴리스 바이너리   build/release/webserver
#   make debug       : 디버그 바이너리   build/debug/webserver   (-O0 -g3)
#   make test        : 단위 테스트 빌드 + 실행 (debug 설정)
#   make asan        : ASan + UBSan 으로 바이너리와 단위 테스트를 빌드하고 테스트 실행
#   make func        : 기능 테스트 (서버를 띄우고 curl 로 검증, tests/func/*.sh)
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

CFLAGS := $(CSTD) $(MODEFLAGS) -Isrc -MMD -MP
LDLIBS := -pthread

# 구현이 끝난 모듈만 나열한다. 빈 모듈은 -pedantic 에서 "빈 번역 단위" 오류가 나므로
# 각 단계에서 구현할 때 여기에 추가한다.
MODULES := buf http_parser mime netio log http_response threadpool server conn static

BUILD     := build/$(MODE)
LIB_OBJS  := $(patsubst %,$(BUILD)/obj/%.o,$(MODULES))
MAIN_OBJ  := $(BUILD)/obj/main.o
BIN       := $(BUILD)/webserver
TEST_SRCS := $(wildcard tests/unit/test_*.c)
TEST_BINS := $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(TEST_SRCS))

.PHONY: all debug asan test func clean bin unit unit-run

all: bin

debug:
	@$(MAKE) --no-print-directory MODE=debug bin

test:
	@$(MAKE) --no-print-directory MODE=debug unit-run

asan:
	@$(MAKE) --no-print-directory MODE=asan bin unit-run SAN=1

# 기능 테스트: 실제 서버를 띄우고 curl 로 검증 (포트와 curl 이 필요해서 test 와 분리)
func: bin
	@set -e; for t in tests/func/*.sh; do case $$t in */lib.sh) continue;; esac; bash $$t $(BIN); done

bin: $(BIN)

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

clean:
	rm -rf build

-include $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_BINS:=.d)
