/*
 * rproxy.c — 리버스 프록시: 경로 기반 라우팅 + 라운드로빈 + 백엔드 health
 *
 * 상태: 빈 모듈 (4단계에서 구현).
 * 빈 번역 단위는 -pedantic 에서 오류이므로, 구현 전까지는 Makefile 의 MODULES 에 넣지 않는다.
 */
#include "rproxy.h"
