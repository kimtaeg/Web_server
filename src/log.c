/*
 * log.c — 레벨별 로그 출력. 여러 워커가 동시에 쓰므로 mutex 로 직렬화한다
 *
 * 상태: 빈 모듈 (0단계에서 구현).
 * 빈 번역 단위는 -pedantic 에서 오류이므로, 구현 전까지는 Makefile 의 MODULES 에 넣지 않는다.
 */
#include "log.h"
