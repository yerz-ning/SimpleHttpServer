/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * 版权所有 (C) 2026 yerz-ning
 * 本文件属于 SimpleHttpServer 项目的一部分
 * 完整许可条款请参阅项目根目录下的 LICENSE 文件
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <pthread.h>

#define BUFFER_SIZE 4096
#define DEFAULT_PORT 80
#define BACKLOG 5
#define MAX_THREADS 16
#define DEFAULT_THREADS 4
#define QUEUE_SIZE 1024
#define STACK_SIZE (256 * 1024)  // 256KB per thread

char *root_dir = ".";
int port = DEFAULT_PORT;
int log_level = 1; // 默认错误级别
int num_threads = DEFAULT_THREADS;

// 线程池全局变量
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;
int queue[QUEUE_SIZE];
int queue_head = 0, queue_tail = 0, queue_count = 0;

// 日志锁
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// 发送全部数据，失败返回 -1
ssize_t send_all(int fd, const void *buf, size_t len) {
    ssize_t total = 0;
    while (total < (ssize_t)len) {
        ssize_t n = send(fd, (char*)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

// 发送响应头
int send_header(int client, int code, const char *status, const char *type,
                long content_length, const char *extra) {
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        code, status, type, content_length, extra ? extra : "");
    if (len < 0 || len >= (int)sizeof(header)) {
        // 头部过长时，发送基本头
        len = snprintf(header, sizeof(header),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "\r\n",
            code, status, type, content_length);
    }
    return send_all(client, header, len);
}

// MIME 类型
const char* get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".mp4") == 0 || strcmp(ext, ".m4v") == 0) return "video/mp4";
    if (strcmp(ext, ".webm") == 0) return "video/webm";
    if (strcmp(ext, ".ogg") == 0) return "video/ogg";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}

// 安全构建路径，防止目录遍历
int build_safe_path(const char *root, const char *url_path, char *out, size_t out_size) {
    char abs_root[1024];
    if (realpath(root, abs_root) == NULL) {
        if (mkdir(root, 0755) != 0 && errno != EEXIST) return -1;
        if (realpath(root, abs_root) == NULL) return -1;
    }

    char combined[2048];
    int len = snprintf(combined, sizeof(combined), "%s%s", abs_root, url_path);
    if (len < 0 || len >= (int)sizeof(combined)) return -1;

    char resolved[2048];
    if (realpath(combined, resolved) == NULL) {
        // 文件/目录不存在，尝试规范化父目录
        char copy[2048];
        strncpy(copy, combined, sizeof(copy)-1);
        copy[sizeof(copy)-1] = '\0';
        char *last_slash = strrchr(copy, '/');
        if (last_slash == NULL) return -1;
        *last_slash = '\0';
        char parent[2048];
        if (realpath(copy, parent) == NULL) return -1;
        size_t root_len = strlen(abs_root);
        if (strncmp(parent, abs_root, root_len) != 0 ||
            (parent[root_len] != '\0' && parent[root_len] != '/')) return -1;
        snprintf(out, out_size, "%s/%s", parent, last_slash + 1);
        return 0;
    }

    size_t root_len = strlen(abs_root);
    if (strncmp(resolved, abs_root, root_len) != 0 ||
        (resolved[root_len] != '\0' && resolved[root_len] != '/')) return -1;

    snprintf(out, out_size, "%s", resolved);
    return 0;
}

// 发送文件完整内容（流式）
void send_file_complete(int client, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_header(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        send_header(client, 500, "Internal Error", "text/html", 0, NULL);
        return;
    }
    if (send_header(client, 200, "OK", get_mime_type(path), st.st_size, NULL) < 0) {
        close(fd);
        return;
    }
    char buf[BUFFER_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (send_all(client, buf, n) < 0) break;
    }
    close(fd);
}

// 发送文件部分内容（Range 请求）
void send_file_range(int client, const char *path, long start, long end) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_header(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        send_header(client, 500, "Internal Error", "text/html", 0, NULL);
        return;
    }
    long file_size = st.st_size;
    if (file_size == 0) {
        close(fd);
        send_header(client, 416, "Range Not Satisfiable", "text/html", 0, NULL);
        return;
    }

    // 规范化 Range
    if (end == -1) end = file_size - 1;
    if (start < 0 || end < 0 || start >= file_size || start > end) {
        close(fd);
        send_header(client, 416, "Range Not Satisfiable", "text/html", 0, NULL);
        return;
    }
    if (end >= file_size) end = file_size - 1;

    long content_length = end - start + 1;
    char extra[128];
    snprintf(extra, sizeof(extra), "Content-Range: bytes %ld-%ld/%ld\r\n", start, end, file_size);

    if (send_header(client, 206, "Partial Content", get_mime_type(path), content_length, extra) < 0) {
        close(fd);
        return;
    }
    if (lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        return;
    }
    char buf[BUFFER_SIZE];
    long remaining = content_length;
    while (remaining > 0) {
        long to_read = (remaining < (long)sizeof(buf)) ? remaining : (long)sizeof(buf);
        ssize_t n = read(fd, buf, to_read);
        if (n <= 0) break;
        if (send_all(client, buf, n) < 0) break;
        remaining -= n;
    }
    close(fd);
}

// 流式目录列表（使用 Content-Length 未知，直接关闭连接）
void list_directory(int client, const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        send_header(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }

    char header[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
    if (send_all(client, header, strlen(header)) < 0) {
        closedir(d);
        return;
    }

    char *head = "<!DOCTYPE html><html><head><title>Index</title></head><body><h2>Index of ";
    send_all(client, head, strlen(head));

    char display_path[1024];
    if (strcmp(path, root_dir) == 0) {
        snprintf(display_path, sizeof(display_path), "/");
    } else {
        snprintf(display_path, sizeof(display_path), "%s/", path + strlen(root_dir));
    }
    send_all(client, display_path, strlen(display_path));

    char *mid = "</h2><ul>";
    send_all(client, mid, strlen(mid));

    struct dirent *entry;
    char fullpath[2048];
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        char line[512];
        if (S_ISDIR(st.st_mode)) {
            snprintf(line, sizeof(line), "<li><a href=\"%s/\">%s/</a></li>", entry->d_name, entry->d_name);
        } else {
            snprintf(line, sizeof(line), "<li><a href=\"%s\">%s</a></li>", entry->d_name, entry->d_name);
        }
        send_all(client, line, strlen(line));
    }
    char *tail = "</ul></body></html>";
    send_all(client, tail, strlen(tail));
    closedir(d);
}

// URL 解码
void url_decode(char *str) {
    char *p = str, *q = str;
    while (*p) {
        if (*p == '%' && isxdigit(*(p+1)) && isxdigit(*(p+2))) {
            int high = tolower(*(p+1));
            int low = tolower(*(p+2));
            int value = (high <= '9' ? high - '0' : high - 'a' + 10) * 16 +
                        (low <= '9' ? low - '0' : low - 'a' + 10);
            *q++ = (char)value;
            p += 3;
        } else if (*p == '+') {
            *q++ = ' ';
            p++;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
}

// 解析 Range 头，支持单个范围，返回 1 表示成功
int parse_range(const char *headers, long *start, long *end) {
    const char *p = strstr(headers, "Range: bytes=");
    if (!p) return 0;
    p += 13;
    char *dash = strchr(p, '-');
    if (!dash) return 0;

    // 忽略多范围，只取第一个
    char *comma = strchr(p, ',');
    if (comma && dash > comma) return 0; // 异常

    *start = atol(p);
    if (*(dash+1) != '\0' && *(dash+1) != ',') {
        *end = atol(dash+1);
    } else {
        *end = -1;
    }
    if (*start < 0 || (*end != -1 && *end < 0)) return 0;
    return 1;
}

// 读取完整 HTTP 头部，直到空行
int read_http_headers(int client, char *buffer, size_t buf_size) {
    size_t total = 0;
    while (total < buf_size - 1) {
        ssize_t n = recv(client, buffer + total, buf_size - 1 - total, 0);
        if (n <= 0) return -1;
        total += n;
        buffer[total] = '\0';
        if (strstr(buffer, "\r\n\r\n") != NULL || strstr(buffer, "\n\n") != NULL) {
            return 0;
        }
    }
    return -1;
}

// 处理单个客户端连接
void handle_request(int client, struct sockaddr_in *addr) {
    char headers[4096];
    if (read_http_headers(client, headers, sizeof(headers)) < 0) {
        close(client);
        return;
    }

    // 解析请求行
    char *line_end = strstr(headers, "\r\n");
    if (line_end == NULL) line_end = strstr(headers, "\n");
    if (line_end == NULL) { close(client); return; }
    char request_line[2048];
    size_t line_len = line_end - headers;
    if (line_len >= sizeof(request_line)) { close(client); return; }
    memcpy(request_line, headers, line_len);
    request_line[line_len] = '\0';

    char *method = strtok(request_line, " ");
    char *path = strtok(NULL, " ");
    if (!method || !path) { close(client); return; }

    // 只处理 GET 和 HEAD 请求
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_header(client, 405, "Method Not Allowed", "text/html", 0, NULL);
        close(client);
        return;
    }

    // 日志
    if (log_level == 0) {
        pthread_mutex_lock(&log_mutex);
        printf("请求 %s %s (来自 %s)\n", method, path, inet_ntoa(addr->sin_addr));
        printf("Request %s %s (from %s)\n", method, path, inet_ntoa(addr->sin_addr));
        pthread_mutex_unlock(&log_mutex);
    }

    // URL 解码
    url_decode(path);

    // 安全构建文件系统路径
    char fullpath[2048];
    if (build_safe_path(root_dir, path, fullpath, sizeof(fullpath)) < 0) {
        send_header(client, 403, "Forbidden", "text/html", 0, NULL);
        close(client);
        return;
    }

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
        list_directory(client, fullpath);
        close(client);
        return;
    }

    long start = 0, end = -1;
    if (parse_range(headers, &start, &end)) {
        send_file_range(client, fullpath, start, end);
    } else {
        send_file_complete(client, fullpath);
    }
    close(client);
}

// 线程工作函数
void *worker_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&queue_mutex);
        while (queue_count == 0) {
            pthread_cond_wait(&queue_not_empty, &queue_mutex);
        }
        int client = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        queue_count--;
        pthread_cond_signal(&queue_not_full);
        pthread_mutex_unlock(&queue_mutex);

        // 获取客户端地址（仅用于日志）
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        getpeername(client, (struct sockaddr*)&addr, &len);
        handle_request(client, &addr);
        // handle_request 内部已关闭 client
    }
    return NULL;
}

// 将客户端 socket 加入队列
void enqueue_client(int client) {
    pthread_mutex_lock(&queue_mutex);
    while (queue_count == QUEUE_SIZE) {
        pthread_cond_wait(&queue_not_full, &queue_mutex);
    }
    queue[queue_tail] = client;
    queue_tail = (queue_tail + 1) % QUEUE_SIZE;
    queue_count++;
    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
}

int main(int argc, char *argv[]) {
    // 忽略 SIGPIPE，防止客户端断开导致进程退出
    signal(SIGPIPE, SIG_IGN);

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i+1 < argc) {
            root_dir = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i+1 < argc) {
            if (strcmp(argv[++i], "info") == 0) log_level = 0;
        } else if (strcmp(argv[i], "-t") == 0 && i+1 < argc) {
            num_threads = atoi(argv[++i]);
            if (num_threads < 1) num_threads = 1;
            if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
        } else {
            fprintf(stderr, "用法: %s [-p port] [-r root_dir] [-l info] [-t threads]\n", argv[0]);
            return 1;
        }
    }

    // 创建监听 socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(sock, BACKLOG) < 0) { perror("listen"); return 1; }

    printf("HTTP 服务器启动 端口 %d 根目录 %s 线程数 %d\n", port, root_dir, num_threads);
    printf("HTTP server started port %d serving %s threads %d\n", port, root_dir, num_threads);
    printf("日志级别: %s\n", log_level == 0 ? "info" : "error");

    // 创建工作线程
    pthread_t threads[MAX_THREADS];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], &attr, worker_thread, NULL) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    pthread_attr_destroy(&attr);

    // 主循环：接受连接并放入队列
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client = accept(sock, (struct sockaddr *)&client_addr, &len);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        enqueue_client(client);
    }

    close(sock);
    return 0;
}
