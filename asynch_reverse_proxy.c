#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include "cache.h"
#include "load_balancer.h"

#define MAX_BUFFER_SIZE 1024
#define MAX_EVENTS 10
#define CHUNK_SIZE 512  // 청크 크기

int epoll_fd; // 전역 변수로 설정

void *handle_request(void *client_sock_ptr);
void send_response(int client_sock, const char *response, int is_head, int response_size);
httpserver select_server_based_on_mode();
int create_server_connection(const char *server_ip, int port);
void send_request_to_server(int server_sock, const char *method, const char *url);
void load_config(const char *config_file);
void send_chunked_response(int client_sock, int server_sock);

int PROXY_PORT;
char TARGET_SERVER1[256];
char TARGET_SERVER2[256];
int TARGET_PORT;
int CACHE_ENABLED;
int LOAD_BALANCER_MODE;

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
            } else if (strcmp(key, "TARGET_SERVER2") == 0) {
                strncpy(TARGET_SERVER2, value, sizeof(TARGET_SERVER2) - 1);
            } else if (strcmp(key, "TARGET_PORT") == 0) {
                TARGET_PORT = atoi(value);
            } else if (strcmp(key, "CACHE_ENABLED") == 0) {
                CACHE_ENABLED = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
            } else if (strcmp(key, "LOAD_BALANCER_MODE") == 0) {
                LOAD_BALANCER_MODE = atoi(value);
            }
        }
    }
    fclose(file);
}

int main() {
    int server_sock;
    struct sockaddr_in server_addr;
    struct epoll_event ev, events[MAX_EVENTS];

    load_config("reverse_proxy.conf");

    httpserver servers[] = {
        {"10.198.138.212", 12345, 3},
        {"10.198.138.213", 12345, 10}
    };
    init_http_servers(servers, 2);
    printf("Selected ip and port : %s, %d\n", servers[0].ip, servers[0].port);

    if (CACHE_ENABLED) {
        cache_init();
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    //포트 재사용
    int optvalue = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optvalue, sizeof(optvalue)) < 0) {
        perror("setsockopt failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PROXY_PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }
    printf("Server listening on port %d...\n", PROXY_PORT);

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev) == -1) {
        perror("epoll_ctl failed");
        close(server_sock);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_sock) {
                int client_sock = accept(server_sock, NULL, NULL);
                if (client_sock == -1) {
                    perror("Accept failed");
                    continue;
                }
                fcntl(client_sock, F_SETFL, O_NONBLOCK);
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_sock;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &ev) == -1) {
                    perror("epoll_ctl failed for client socket");
                    close(client_sock);
                }
            } else {
                handle_request((void *)&events[i].data.fd);
            }
        }
    }
    close(server_sock);
    close(epoll_fd);
    return 0;
}

void send_chunked_response(int client_sock, int server_sock) {
    char chunk[CHUNK_SIZE];
    ssize_t bytes_received, bytes_sent;

    while (1) {
        bytes_received = recv(server_sock, chunk, sizeof(chunk), 0);
        if (bytes_received <= 0) {
            break;  // 서버로부터 더 이상 데이터가 없으면 종료
        }

        // 청크 헤더 (길이 정보) 전송
        char chunk_header[64];
        int chunk_header_len = snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", bytes_received);
        send(client_sock, chunk_header, chunk_header_len, 0);

        // 청크 데이터 전송
        bytes_sent = 0;
        while (bytes_sent < bytes_received) {
            ssize_t sent = send(client_sock, chunk + bytes_sent, bytes_received - bytes_sent, 0);
            if (sent <= 0) {
                break;
            }
            bytes_sent += sent;
        }

        // 청크 끝을 나타내는 "\r\n" 전송
        send(client_sock, "\r\n", 2, 0);
    }

    // 마지막 청크 전송
    send(client_sock, "0\r\n\r\n", 5, 0);
}

void send_response(int client_sock, const char *response, int is_head, int response_size) {
    char header[MAX_BUFFER_SIZE];
    int header_size = 0;

    // HTTP/1.1 응답 헤더 추가
    if (is_head) {
        header_size = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", response_size);
    } else {
        header_size = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", response_size);
    }

    // 먼저 응답 헤더 전송
    ssize_t bytes_sent = send(client_sock, header, header_size, 0);
    if (bytes_sent == -1) {
        if (errno == EPIPE || errno == ECONNRESET) {
            // 클라이언트가 연결을 끊었을 때의 처리
            perror("Broken pipe or connection reset");
            close(client_sock);
            return;
        } else {
            perror("Send header failed");
            close(client_sock);
            return;
        }
    }

    ssize_t total_sent = 0;

    // 응답 본문 전송
    while (total_sent < response_size) {
        bytes_sent = send(client_sock, response + total_sent, response_size - total_sent, 0);
        
        if (bytes_sent == -1) {
            if (errno == EPIPE || errno == ECONNRESET) {
                // 클라이언트가 연결을 끊었을 때의 처리
                perror("Broken pipe or connection reset");
                close(client_sock);
                return;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 비동기 대기
                struct epoll_event ev;
                ev.events = EPOLLOUT | EPOLLET;  // EPOLLOUT 이벤트로 설정
                ev.data.fd = client_sock;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_sock, &ev) == -1) {
                    perror("epoll_ctl failed");
                    close(client_sock);
                }
                return;
            } else {
                perror("Send failed");
                close(client_sock);
                return;
            }
        }

        total_sent += bytes_sent;
    }

    // 응답 완료 후 EPOLLIN 이벤트로 복구
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET; 
    ev.data.fd = client_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_sock, &ev) == -1) {
        perror("epoll_ctl failed");
        close(client_sock);
    }
}


int create_server_connection(const char *server_ip, int port) {
    // 로드 밸런서에 따른 서버 선택
    httpserver server = select_server_based_on_mode();  // 여기서 서버 선택
    printf("Selected server: %s:%d\n", server.ip, server.port);

    int server_sock;
    struct sockaddr_in server_addr;

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Server socket creation failed");
        return -1;
    }

    if (fcntl(server_sock, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl failed to set non-blocking mode");
        close(server_sock);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server.port);  // 로드 밸런서에서 선택된 포트를 사용

    if (inet_pton(AF_INET, server.ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid IP address");
        close(server_sock);
        return -1;
    }

    if (connect(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            perror("Connection failed");
            close(server_sock);
            return -1;
        }
    }

    struct epoll_event ev;
    ev.events = EPOLLOUT | EPOLLET;  // EPOLLOUT 이벤트로 등록
    ev.data.fd = server_sock;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev) == -1) {
        perror("epoll_ctl failed");
        close(server_sock);
        return -1;
    }

    return server_sock;
}

void send_request_to_server(int server_sock, const char *method, const char *url) {
    char request[MAX_BUFFER_SIZE];
    snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n", method, url);

    ssize_t bytes_sent = send(server_sock, request, strlen(request), 0);
    if (bytes_sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        struct epoll_event ev;
        ev.events = EPOLLOUT | EPOLLET; 
        ev.data.fd = server_sock;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, server_sock, &ev) == -1) {
            perror("epoll_ctl failed");
        }
    }

    printf("Request sent to server: %s\n", request);

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_sock;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, server_sock, &ev) == -1) {
        perror("epoll_ctl failed");
    }
}


httpserver select_server_based_on_mode() {
    httpserver server;
    if (LOAD_BALANCER_MODE == 0) {
        server = round_robin();
        printf("Load Balancer Mode: Round Robin\n");
    } else if (LOAD_BALANCER_MODE == 1) {
        server = weighted_round_robin();
        printf("Load Balancer Mode: Weighted Round Robin\n");
    } else if (LOAD_BALANCER_MODE == 2) {
        server = least_connection();
        printf("Load Balancer Mode: Least Connection\n");
    } else {
        // 로드 밸런서를 사용하지 않는 경우 첫 번째 서버로 고정
        strcpy(server.ip, "10.198.138.212");
        server.port = 12345;
        printf("Load Balancer Mode: None (Fixed server selected)\n");
    }

    return server;
}

void handle_http_version_compatibility(int server_sock, int client_sock) {
    char buffer[MAX_BUFFER_SIZE];
    ssize_t bytes_received;

    // 서버에서 응답을 받을 때, HTTP/0.9 응답이 올 경우 HTTP/1.1로 변환
    bytes_received = recv(server_sock, buffer, sizeof(buffer), 0);
    if (bytes_received <= 0) {
        perror("Failed to receive data from server");
        return;
    }

    // 응답이 HTTP/0.9일 경우, HTTP/1.1로 변환
    if (bytes_received > 0 && buffer[0] == 'H' && buffer[1] == 'T' && buffer[2] == 'T' && buffer[3] == 'P') {
        // 만약 HTTP/0.9인 경우 (버전이 없을 수 있음), 버전 추가
        if (strstr(buffer, "HTTP/0.9") != NULL) {
            // HTTP/1.1로 헤더 수정
            char new_buffer[MAX_BUFFER_SIZE];
            snprintf(new_buffer, sizeof(new_buffer), "HTTP/1.1 200 OK\r\n%s", buffer + 8);  // "HTTP/0.9"에서 "HTTP/1.1"로 바꾸기
            send(client_sock, new_buffer, strlen(new_buffer), 0);
        } else {
            // HTTP/1.1로 응답 그대로 전달
            send(client_sock, buffer, bytes_received, 0);
        }
    }
}

void *handle_request(void *client_sock_ptr) {
    int client_sock = *(int *)client_sock_ptr;
    char buffer[MAX_BUFFER_SIZE];
    ssize_t bytes_received;

    // 클라이언트로부터 데이터를 받기 위해 EPOLLIN 이벤트를 기다림
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // 읽기 이벤트와 에지 트리거 설정
    ev.data.fd = client_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_sock, &ev) == -1) {
        perror("epoll_ctl failed");
        close(client_sock);
        return NULL;
    }

    // 비동기적으로 데이터를 받을 때까지 대기
    while (1) {
        bytes_received = recv(client_sock, buffer, sizeof(buffer), 0);
        if (bytes_received > 0) {
            // 디버깅: 수신된 요청 출력
            printf("Request content:\n%.*s\n", (int)bytes_received, buffer);

            // GET, HEAD 메서드 및 URL, 프로토콜 추출
            char method[16], url[256], protocol[16];
            if (sscanf(buffer, "%s %s %s", method, url, protocol) != 3) {
                fprintf(stderr, "Failed to parse the request line properly\n");
                close(client_sock);
                return NULL;
            }

            // 디버깅: 파싱된 결과 출력
            printf("Parsed method: %s, URL: %s, Protocol: %s\n", method, url, protocol);

            // URL 유효성 검사
            if ((strcmp(method, "GET") != 0) && (strcmp(method, "HEAD") != 0)) {
                fprintf(stderr, "Invalid request method: %s\n", method);
                close(client_sock);
                return NULL;
            }

            httpserver server = round_robin();  // 로드밸런서 호출
            printf("Forwarding to server: %s:%d\n", server.ip, server.port);

            // 서버에 연결
            int server_sock = create_server_connection(server.ip, server.port);
            if (server_sock < 0) {
                send_response(client_sock, "HTTP/1.1 500 Internal Server Error\r\n", 0, 39);
                close(client_sock);
                return NULL;
            }

            // 서버에 요청 보내기
            send_request_to_server(server_sock, method, url);

            // 서버로부터 비동기 응답을 받기 위해 EPOLLIN 이벤트를 대기
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = server_sock;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, server_sock, &ev) == -1) {
                perror("epoll_ctl failed");
                close(server_sock);
                close(client_sock);
                return NULL;
            }

            // 서버 응답을 받아 클라이언트에게 전달하기 전에 버전 호환성 처리
            handle_http_version_compatibility(server_sock, client_sock);

            // 서버 응답을 마친 후 소켓 종료
            close(client_sock);
            close(server_sock);
            break;
        } else if (bytes_received == 0) {
            // 연결 종료 처리
            close(client_sock);
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 데이터가 없으면 비동기 대기
                continue;
            } else {
                perror("recv failed");
                close(client_sock);
                return NULL;
            }
        }
    }

    return NULL;
}

