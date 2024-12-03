#ifndef CACHE_H
#define CACHE_H

#define MAX_BUFFER_SIZE 1024

void cache_init();
int cache_lookup(const char *url, char *data);
void cache_store(const char *url, const char *data);

#endif

