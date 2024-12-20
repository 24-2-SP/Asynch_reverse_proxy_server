#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "load_balancer.h"

#define MAX_HTTP_SERVERS 10

static httpserver http_servers[MAX_HTTP_SERVERS];
static int http_server_count = 0;
static int current_index = 0;

void init_http_servers(httpserver servers[], int count) {
    if (count > MAX_HTTP_SERVERS) {
        fprintf(stderr, "Error: 최대 %d개의 서버만 사용 가능합니다.\n", MAX_HTTP_SERVERS);
        return;
    }
    
    for (int i = 0; i < count; i++) {
        strcpy(http_servers[i].ip, servers[i].ip);
        http_servers[i].port = servers[i].port;
        http_servers[i].weight = servers[i].weight;
        http_servers[i].is_healthy = 1;
        http_servers[i].current_weight = 0;
    }

    http_server_count = count;
    current_index = 0;
}

httpserver round_robin() {
    if(http_server_count == 0) {
        fprintf(stderr, "사용 가능한 http 서버가 없습니다.\n");
        httpserver empty = {"", 0, 0};
        return empty;
    }

    httpserver server = http_servers[current_index];
    current_index = (current_index + 1) % http_server_count;
    return server;
}

httpserver weighted_round_robin() {
    int total_weight = 0;
    httpserver *selected_server = NULL;

    for (int i = 0; i < http_server_count; i++) {
        if (http_servers[i].is_healthy) {
            total_weight += http_servers[i].weight;
            http_servers[i].current_weight += http_servers[i].weight;

            if (selected_server == NULL || http_servers[i].current_weight > selected_server->current_weight) {
                selected_server = &http_servers[i];
            }
        }
    }

    if (selected_server == NULL) {
        fprintf(stderr, "No healthy servers available\n");
        exit(EXIT_FAILURE);
    }

    // 선택된 서버의 가중치를 감소
    selected_server->current_weight -= total_weight;

    return *selected_server;
}

httpserver least_connection() {
    if(http_server_count == 0) {
        fprintf(stderr, "사용 가능한 http 서버가 없습니다.\n");
        httpserver empty = {"", 0};
        return empty;
    }

    int min_connections = http_servers[0].active_connections;
    int min_index = 0;
    for (int i = 1; i < http_server_count; i++) {
        if (http_servers[i].active_connections < min_connections) {
            min_connections = http_servers[i].active_connections;
            min_index = i;
        }
    }

    http_servers[min_index].active_connections += 1;
    if (http_servers[min_index].active_connections < 0) {
        http_servers[min_index].active_connections = 0;
    }

    return http_servers[min_index];
}

httpserver (*load_balancer_select(int mode))() {
    switch (mode) {
        case 0:
            return round_robin;
        case 1:
            return weighted_round_robin;
        case 2:
            return least_connection;
        default:
            fprintf(stderr, "Invalid load balancer mode!\n");
            exit(EXIT_FAILURE);
    }
}
