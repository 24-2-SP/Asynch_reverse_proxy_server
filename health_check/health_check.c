#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "health_check.h"

#define HEALTH_CHECK_INTERVAL 5

void *health_check(void *args) {
    health_check_args *check_args = (health_check_args *)args;
    httpserver *servers = check_args->servers;
    int server_count = check_args->server_count;
    while (1) {
        for (int i = 0; i < server_count; i++) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                perror("Socket creation failed for health check");
                continue;
            }

            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(servers[i].port);
            inet_pton(AF_INET, servers[i].ip, &addr.sin_addr);

            // 서버 연결 확인
            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                servers[i].is_healthy = 1;
                printf("Server %s:%d is healthy\n", servers[i].ip, servers[i].port);
            } else {
                servers[i].is_healthy = 0;
                printf("Server %s:%d is unhealthy\n", servers[i].ip, servers[i].port);
            }

            close(sock);
        }

        // HEALTH_CHECK_INTERVAL 초 동안 대기
        sleep(HEALTH_CHECK_INTERVAL);
    }

    return NULL;
}