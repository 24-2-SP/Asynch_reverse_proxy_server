#ifndef HEALTH_CHECK_H
#define HEALTH_CHECK_H

#include "../load_balancer/load_balancer.h"

typedef struct {
    httpserver *servers;
    int server_count;
} health_check_args;

void *health_check(void *args);

#endif