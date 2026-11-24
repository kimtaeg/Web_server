# 루트 Makefile — 두 구현에 위임만 한다.
#   threadpool/ : accept 스레드 + 워커 스레드 풀 (정적·CGI·리버스/포워드 프록시·캐시)
#   multiplex/  : 단일 스레드 이벤트 루프, epoll(Linux)/kqueue(macOS) (정적 파일)
#
#   make <타깃>            : 두 구현 모두에서 <타깃> 실행 (all debug test asan func func-asan valgrind leakcheck clean)
#   make -C threadpool ... : 한쪽만
#   make load              : 두 서버를 같은 조건으로 부하 비교 (wrk 필요, docs/benchmark.md 갱신)
#
# 각 폴더는 독립적으로 빌드된다 (완전 복사 두 벌: 한쪽을 고쳐도 다른 쪽에 영향이 없다).

IMPLS := $(wildcard threadpool multiplex)
TARGETS := all debug test asan func func-asan valgrind leakcheck clean

.PHONY: $(TARGETS) load

$(TARGETS):
	@set -e; for d in $(IMPLS); do echo "==== [$$d] make $@ ===="; $(MAKE) --no-print-directory -C $$d $@; done

load:
	@for d in $(IMPLS); do $(MAKE) --no-print-directory -C $$d all; done
	@bash tests/load/run.sh
