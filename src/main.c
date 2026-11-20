/*
 * main.c — 진입점.
 *
 * 상태: 0단계 껍데기. 2단계에서 시그널 설정(SIGPIPE 무시, 종료 플래그) →
 * config 로드 → server 실행 순서로 채운다.
 */
#include <stdio.h>

int main(void)
{
    fprintf(stderr, "webserver: 아직 구현되지 않음 (0단계)\n");
    return 1;
}
