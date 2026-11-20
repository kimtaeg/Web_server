/*
 * router.c — 요청 경로 → static | cgi | rproxy | fproxy 핸들러 선택
 *
 * 상태: 빈 모듈 (3단계에서 구현).
 * 빈 번역 단위는 -pedantic 에서 오류이므로, 구현 전까지는 Makefile 의 MODULES 에 넣지 않는다.
 */
#include "router.h"
