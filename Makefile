CC      ?= cc
OPT     ?= -O2
CFLAGS  += $(OPT) -g -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wno-unused-parameter -pthread -D_POSIX_C_SOURCE=200809L
LDFLAGS += -pthread

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  CFLAGS += -D_DARWIN_C_SOURCE
else
  CFLAGS += -D_DEFAULT_SOURCE
endif

BUILD   ?= build
LIB_SRC := buffer http mime router cache config netutil log threadpool response \
           static_file cgi proxy handler server
LIB_OBJ := $(addprefix $(BUILD)/obj/,$(addsuffix .o,$(LIB_SRC)))
TEST_SRC := $(wildcard tests/unit/*.c)
TEST_OBJ := $(patsubst tests/unit/%.c,$(BUILD)/obj/test_%.o,$(TEST_SRC))

SERVER  := $(BUILD)/webserv
BACKEND := $(BUILD)/echo_backend
TESTBIN := $(BUILD)/unit_tests

.PHONY: all test unit functional load memcheck asan clean run demo

all: $(SERVER) $(BACKEND)

$(BUILD)/obj:
	@mkdir -p $@

$(BUILD)/obj/%.o: src/%.c src/*.h | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/test_%.o: tests/unit/%.c tests/unit/*.h src/*.h | $(BUILD)/obj
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/obj/main.o: src/main.c src/*.h | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(SERVER): $(LIB_OBJ) $(BUILD)/obj/main.o
	$(CC) $(LDFLAGS) $^ -o $@

$(BACKEND): tools/echo_backend.c $(BUILD)/obj/http.o $(BUILD)/obj/buffer.o $(BUILD)/obj/netutil.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(TESTBIN): $(TEST_OBJ) $(LIB_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

unit: $(TESTBIN)
	./$(TESTBIN)

# functional.sh also runs the raw-socket edge cases (tests/edge_cases.py).
test: unit functional

functional: all
	./tests/functional.sh $(BUILD)

load: all
	./tests/load.sh $(BUILD)

memcheck: all
	./tests/memcheck.sh $(BUILD)

# AddressSanitizer + UndefinedBehaviorSanitizer build in its own directory.
asan:
	$(MAKE) BUILD=build-asan OPT="-O1 -fno-omit-frame-pointer -fsanitize=address,undefined" \
	        LDFLAGS="-pthread -fsanitize=address,undefined" all build-asan/unit_tests

run: all
	./$(SERVER) -c conf/webserv.conf

demo: all
	./tests/demo.sh $(BUILD)

clean:
	rm -rf build build-asan
