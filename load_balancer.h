#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include <stdio.h>
#include <string.h>

typedef struct httpserver {
    char ip[16];
    int port;
    int weight;
    int active_connections;
    int is_healthy;
} httpserver;

// 초기화 함수
void init_http_servers(httpserver servers[], int count);

// 로드 밸런싱 전략 함수 선언
httpserver round_robin();
httpserver weighted_round_robin();
httpserver least_connection();

// 모드에 따른 로드 밸런싱 함수 포인터 반환
httpserver (*load_balancer_select(int mode))();

#endif

