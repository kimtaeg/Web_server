/*
 * conn.c — 연결 하나의 keep-alive 루프: 읽기 → 파싱 → 라우팅 → 응답, 유휴 타임아웃
 *
 * 상태: 빈 모듈 (2단계에서 구현).
 * 빈 번역 단위는 -pedantic 에서 오류이므로, 구현 전까지는 Makefile 의 MODULES 에 넣지 않는다.
 */
#include "conn.h"
