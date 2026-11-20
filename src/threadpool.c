/*
 * threadpool.c — bounded queue + pthread 워커 풀. 큐가 가득 차면 호출자가 503 처리
 *
 * 상태: 빈 모듈 (2단계에서 구현).
 * 빈 번역 단위는 -pedantic 에서 오류이므로, 구현 전까지는 Makefile 의 MODULES 에 넣지 않는다.
 */
#include "threadpool.h"
