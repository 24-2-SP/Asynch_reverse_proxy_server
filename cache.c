#include "cache.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define CACHE_SIZE 5

typedef struct {
    char url[256];
    char data[MAX_BUFFER_SIZE];
} CacheEntry;

static CacheEntry cache[CACHE_SIZE];
static int cache_index = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;  // 뮤텍스 추가

// 캐시 초기화
void cache_init() {
    memset(cache, 0, sizeof(cache));
}

// 캐시에서 URL에 해당하는 데이터를 찾아서 반환
int cache_lookup(const char *url, char *data) {
    pthread_mutex_lock(&cache_mutex);  // 캐시 접근 전에 락

    for (int i = 0; i < CACHE_SIZE; i++) {
        if (strcmp(cache[i].url, url) == 0) {
            strcpy(data, cache[i].data);
            pthread_mutex_unlock(&cache_mutex);  // 캐시 접근 후 락 해제
            return 1;  // 캐시에서 데이터를 찾은 경우
        }
    }

    pthread_mutex_unlock(&cache_mutex);  // 캐시 접근 후 락 해제
    return 0;  // 캐시에서 데이터 미발견
}

// 캐시에 URL과 데이터를 저장
void cache_store(const char *url, const char *data) {
    pthread_mutex_lock(&cache_mutex);  // 캐시 접근 전에 락

    strcpy(cache[cache_index].url, url);
    strncpy(cache[cache_index].data, data, MAX_BUFFER_SIZE);

    cache_index = (cache_index + 1) % CACHE_SIZE;  // 캐시 인덱스를 순환

    pthread_mutex_unlock(&cache_mutex);  // 캐시 접근 후 락 해제
}

