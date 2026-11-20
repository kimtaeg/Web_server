/*
 * http_parser.c — I/O 없는 증분 HTTP/1.1 요청 파서 (PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR)
 *
 * 상태: 빈 모듈 (1단계에서 구현).
 * 빈 번역 단위는 -pedantic 에서 오류이므로, 구현 전까지는 Makefile 의 MODULES 에 넣지 않는다.
 */
#include "http_parser.h"
