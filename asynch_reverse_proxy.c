#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/epoll.h>
#include "cache.h"
#include "load_balancer.h"

#define MAX_BUFFER_SIZE 1024
#define MAX_EVENTS 100
#define THREAD_POOL_SIZE 8  // 스레드 풀 크기
#define QUEUE_SIZE 10000      // 작업 큐 크기
#define HEALTH_CHECK_INTERVAL 5

// 작업 큐 구조체
typedef struct {
    int client_sockets[QUEUE_SIZE];
    int front;
    int rear;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} task_queue_t;

httpserver servers[] = {
    {"10.198.138.212", 12345, 3, 1},
    {"10.198.138.213", 12345, 10, 1}
};

int server_count = 2;

// 전역 작업 큐 선언
task_queue_t task_queue = {
    .front = 0,
    .rear = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER
};

// 함수 프로토타입
void *worker_thread(void *arg);
void enqueue_task(int client_sock);
int dequeue_task();

void *handle_request(void *client_sock_ptr);
void send_response(int client_sock, const char *response_header, const char *response_body, int is_head, int response_size);

// 전역 변수로 설정 값 선언
int PROXY_PORT;
char TARGET_SERVER1[256];
char TARGET_SERVER2[256];
int TARGET_PORT;
int CACHE_ENABLED;

void *health_check(void *arg) {
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

// 로드 밸런서 수정: Health Check 기반 선택
httpserver weighted_round_robin() {
    int total_weight = 0;
    httpserver *selected_server = NULL;

    for (int i = 0; i < server_count; i++) {
        if (servers[i].is_healthy) {
            total_weight += servers[i].weight;
            servers[i].current_weight += servers[i].weight;

            if (selected_server == NULL || servers[i].current_weight > selected_server->current_weight) {
                selected_server = &servers[i];
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

// 설정 파일에서 값을 읽어오는 함수
void load_config(const char *config_file) {
    FILE *file = fopen(config_file, "r");
    if (!file) {
        perror("Failed to open config file");
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char key[256], value[256];
        if (sscanf(line, "%255[^=]=%255[^\n]", key, value) == 2) {
            if (strcmp(key, "PROXY_PORT") == 0) {
                PROXY_PORT = atoi(value);
            } else if (strcmp(key, "TARGET_SERVER1") == 0) {
                strncpy(TARGET_SERVER1, value, sizeof(TARGET_SERVER1) - 1);
                TARGET_SERVER1[sizeof(TARGET_SERVER1) - 1] = '\0';
            } else if (strcmp(key, "TARGET_SERVER2") == 0) {
                strncpy(TARGET_SERVER2, value, sizeof(TARGET_SERVER2) - 1);
                TARGET_SERVER2[sizeof(TARGET_SERVER2) - 1] = '\0';
            } else if (strcmp(key, "TARGET_PORT") == 0) {
                TARGET_PORT = atoi(value);
            } else if (strcmp(key, "CACHE_ENABLED") == 0) {
                CACHE_ENABLED = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
            }
        }
    }
    fclose(file);
}

int main() {
    int server_sock, client_sock, epoll_fd;
    struct sockaddr_in server_addr;
    struct epoll_event ev, events[MAX_EVENTS];

    // 설정 파일 읽기
    load_config("reverse_proxy.conf");

    init_http_servers(servers, 2);

        pthread_t health_thread;
    if (pthread_create(&health_thread, NULL, health_check, NULL) != 0) {
        perror("Failed to create health check thread");
        exit(EXIT_FAILURE);
    }
    pthread_detach(health_thread);

    // 캐시 초기화
    if (CACHE_ENABLED) {
        cache_init();
    }

    // 서버 소켓 생성
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PROXY_PORT);

    // 서버 바인딩
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // 서버 리슨
    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PROXY_PORT);

    // epoll 생성
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // 서버 소켓을 epoll에 등록
    ev.events = EPOLLIN;
    ev.data.fd = server_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev) == -1) {
        perror("epoll_ctl failed");
        close(server_sock);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    // 스레드 풀 초기화
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            perror("Thread creation failed");
            close(server_sock);
            close(epoll_fd);
            exit(EXIT_FAILURE);
        }
    }

    while (1) {
        // 이벤트 대기
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 5000);
        if (nfds == -1) {
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_sock) {
                // 새로운 클라이언트 연결 수락
                client_sock = accept(server_sock, NULL, NULL);
                if (client_sock == -1) {
                    perror("Accept failed");
                    continue;
                }

                // 작업 큐에 클라이언트 소켓 추가
                enqueue_task(client_sock);
            }
        }
    }

    close(server_sock);
    close(epoll_fd);
    return 0;
}

// 작업 큐에 작업 추가
void enqueue_task(int client_sock) {
    pthread_mutex_lock(&task_queue.mutex);

    if ((task_queue.rear + 1) % QUEUE_SIZE == task_queue.front) {
        fprintf(stderr, "Task queue is full, dropping connection.\n");
        close(client_sock);
    } else {
        task_queue.client_sockets[task_queue.rear] = client_sock;
        task_queue.rear = (task_queue.rear + 1) % QUEUE_SIZE;
        pthread_cond_signal(&task_queue.cond);
    }

    pthread_mutex_unlock(&task_queue.mutex);
}

// 작업 큐에서 작업 제거
int dequeue_task() {
    pthread_mutex_lock(&task_queue.mutex);

    while (task_queue.front == task_queue.rear) {
        pthread_cond_wait(&task_queue.cond, &task_queue.mutex);
    }

    int client_sock = task_queue.client_sockets[task_queue.front];
    task_queue.front = (task_queue.front + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(&task_queue.mutex);
    return client_sock;
}

// 워커 스레드
void *worker_thread(void *arg) {
    while (1) {
        int client_sock = dequeue_task();
        handle_request(&client_sock);
        close(client_sock);
    }
    return NULL;
}

void *handle_request(void *client_sock_ptr) {
    int client_sock = *(int *)client_sock_ptr;
    int server_sock;
    struct sockaddr_in target_addr;
    char buffer[MAX_BUFFER_SIZE] = {0};
    char method[10] = {0};
    char url[256] = {0};
    char protocol[10] = {0};

    ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
        perror("Failed to read client request");
        close(client_sock);
        return NULL;
    }

    // 디버깅: 수신된 요청 출력
    printf("Request content:\n%.*s\n", (int)bytes_read, buffer);

    // GET, HEAD 메서드 및 URL, 프로토콜 추출
    if (sscanf(buffer, "%s %s %s", method, url, protocol) != 3) {
        fprintf(stderr, "Failed to parse the request line properly\n");
        close(client_sock);
        return NULL;
    }

    httpserver server = weighted_round_robin();  // 로드밸런서 호출
    printf("Forwarding to server: %s:%d\n", server.ip, server.port);

    // 디버깅: 파싱된 결과 출력
    printf("Parsed method: %s, URL: %s, Protocol: %s\n", method, url, protocol);

    // URL 유효성 검사
    if ((strcmp(method, "GET") != 0) && (strcmp(method, "HEAD") != 0)) {
        fprintf(stderr, "Invalid request method: %s\n", method);
        close(client_sock);
        return NULL;
    }

    char cached_data[MAX_BUFFER_SIZE] = {0};
    int header_size = 0;
    char header_buffer[MAX_BUFFER_SIZE] = {0};
    int newline_count = 0;
        if (CACHE_ENABLED && cache_lookup(url, cached_data)) {
        // 캐시 히트 메시지 출력
        printf("Cache hit for URL: %s\n\n\n", url);

        for (int i = 0; i < sizeof(cached_data); i++) {
            header_size++;
            if (cached_data[i] == '\n') {
                newline_count++;
                if (newline_count == 4) {
                    // 4번째 '\n'까지의 크기를 반환
                    break;
                }
            }
        }

        // 헤더와 본문을 분리
        memcpy(header_buffer, cached_data, header_size);

        send_response(client_sock, header_buffer, cached_data, strcmp(method, "HEAD") == 0, strlen(cached_data));  // HEAD일 경우 본문 제외

    } else {
        // 캐시 미스 처리
        printf("Cache miss for URL: %s\n\n\n", url);

        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) {
            perror("Socket creation failed");
            close(client_sock);
            return NULL;
        }

        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(server.port);
        inet_pton(AF_INET, server.ip, &target_addr.sin_addr);

        if (connect(server_sock, (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0) {
            perror("Connection failed");
            close(client_sock);
            close(server_sock);
            return NULL;
        }

        // 요청 전달
        if (write(server_sock, buffer, bytes_read) < 0) {
            perror("Failed to forward request");
            close(client_sock);
            close(server_sock);
            return NULL;
        }

        // 응답 수신 및 스트리밍 방식으로 클라이언트로 전달
        ssize_t bytes_received;
        char response_buffer[MAX_BUFFER_SIZE] = {0};
                int response_size = 0;

        while ((bytes_received = read(server_sock, response_buffer + response_size, sizeof(response_buffer) - response_size)) > 0) {
            response_size += bytes_received;
        }

                for (int i = 0; i < sizeof(response_buffer); i++) {
            header_size++;
            if (response_buffer[i] == '\n') {
                newline_count++;
                if (newline_count == 4) {
                    // 4번째 '\n'까지의 크기를 반환
                    break;
                }
            }
        }

                // 헤더와 본문을 분리
        memcpy(header_buffer, response_buffer, header_size);

        if (bytes_received < 0) {
            perror("Failed to receive response from backend server");
        } else {
            // 캐시에 응답 저장 (응답 크기 검사)
            if (CACHE_ENABLED) {
                printf("Store response for URL: %s into cache\n\n\n", url);
                cache_store(url, response_buffer);
            }

            // 클라이언트로 응답 전송
       send_response(client_sock, header_buffer, response_buffer + header_size, strcmp(method, "HEAD") == 0, response_size - header_size);

        }

        close(server_sock);
    }

    close(client_sock);
    return NULL;
}

void send_response(int client_sock, const char *response_header, const char *response_body, int is_head, int response_size) {
    // HTTP/1.1 응답 헤더 작성 (Content-Type은 텍스트로 설정)
//    const char *header_format = "HTTP/1.1 200 OK\r\n"
//                                "Content-Type: text/plain\r\n"  // 응답의 타입 설정
//                                "Connection: close\r\n"         // 연결 종료 헤더
//                                "Content-Length: %d\r\n"        // 본문 길이
//                                "\r\n";  // 헤더와 본문 구분

    // 응답 헤더의 크기를 계산하여 Content-Length에 설정
    char header[512];
    snprintf(header, sizeof(header), response_header, response_size);

    // 헤더 전송
    if (write(client_sock, header, strlen(header)) < 0) {
        perror("Failed to send header");
        return;
    }

    // 본문이 있으면 본문 전송
    if (!is_head) {
        if (write(client_sock, response_body, response_size) < 0) {
            perror("Failed to send response body");
        }
    }
}