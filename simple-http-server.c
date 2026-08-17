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

#define BUFFER_SIZE 4096
#define DEFAULT_PORT 80
#define BACKLOG 5

char *root_dir = ".";
int port = DEFAULT_PORT;
int log_level = 0; // 0 = info, 1 = error

// 将缓冲区数据全部发送到套接字描述符
ssize_t send_all(int fd, const void *buf, size_t len) {
    ssize_t total = 0;
    while (total < (ssize_t)len) {
        ssize_t n = send(fd, (char*)buf + total, len - total, 0);
        if (n <= 0) return total;
        total += n;
    }
    return total;
}

// 向客户端发送 HTTP 响应头
void send_header(int client, int code, const char *status, const char *type,
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
    send_all(client, header, len);
}

// 根据文件扩展名返回对应的 MIME 类型
const char* get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    return "application/octet-stream";
}

// 响应 Range 请求，发送文件的指定字节范围
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
    if (end == -1) end = file_size - 1;
    if (start > end || start >= file_size) {
        close(fd);
        send_header(client, 416, "Range Not Satisfiable", "text/html", 0, NULL);
        return;
    }
    if (end >= file_size) end = file_size - 1;

    long content_length = end - start + 1;
    char extra[128];
    snprintf(extra, sizeof(extra), "Content-Range: bytes %ld-%ld/%ld\r\n", start, end, file_size);
    send_header(client, 206, "Partial Content", get_mime_type(path), content_length, extra);

    lseek(fd, start, SEEK_SET);
    char buf[BUFFER_SIZE];
    long remaining = content_length;
    while (remaining > 0) {
        long to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        ssize_t n = read(fd, buf, to_read);
        if (n <= 0) break;
        send_all(client, buf, n);
        remaining -= n;
    }
    close(fd);
}

// 以流式方式发送完整的文件内容
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

    send_header(client, 200, "OK", get_mime_type(path), st.st_size, NULL);
    char buf[BUFFER_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        send_all(client, buf, n);
    }
    close(fd);
}

// 流式生成指定目录的 HTML 文件列表
void list_directory(int client, const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        send_header(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }

    const char *head = "<!DOCTYPE html><html><head><meta charset=utf-8><title>File list</title></head><body><h2>Index of ";
    send_all(client, head, strlen(head));
    send_all(client, path, strlen(path));
    const char *mid = "</h2><ul>";
    send_all(client, mid, strlen(mid));

    struct dirent *entry;
    char fullpath[512];
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
    const char *tail = "</ul></body></html>";
    send_all(client, tail, strlen(tail));
    closedir(d);
}

// 从请求头中解析 Range 字段
long parse_range(const char *headers, long *start, long *end) {
    const char *range_header = strstr(headers, "Range: bytes=");
    if (!range_header) return 0;
    range_header += 13;
    char *dash = strchr(range_header, '-');
    if (!dash) return 0;
    *start = atol(range_header);
    if (*(dash+1) != '\0') {
        *end = atol(dash+1);
    } else {
        *end = -1;
    }
    return 1;
}

// 处理单个 HTTP 请求，根据路径类型分发到不同处理函数
void handle_request(int client, struct sockaddr_in *client_addr) {
    char buf[BUFFER_SIZE];
    int n = recv(client, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        if (log_level == 0) {
            printf("连接关闭或读取失败\n");
            printf("Connection closed or read failed\n");
        }
        return;
    }
    buf[n] = '\0';

    char *method = strtok(buf, " ");
    char *path = strtok(NULL, " ");
    if (!method || !path) {
        if (log_level == 0) {
            printf("无效请求\n");
            printf("Invalid request\n");
        }
        return;
    }

    char headers[2048] = {0};
    int total = 0;
    while (1) {
        n = recv(client, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        total += n;
        if (total >= (int)sizeof(headers)-1) break;
        strcat(headers, buf);
        if (strstr(headers, "\r\n\r\n") || strstr(headers, "\n\n")) break;
    }

    char fullpath[1024];
    if (strcmp(path, "/") == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s", root_dir);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s", root_dir, path);
    }

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (log_level == 0) {
            printf("来自 %s 请求目录 %s\n", inet_ntoa(client_addr->sin_addr), path);
            printf("Request from %s for directory %s\n", inet_ntoa(client_addr->sin_addr), path);
        }
        list_directory(client, fullpath);
        return;
    }

    long start = 0, end = -1;
    if (parse_range(headers, &start, &end)) {
        if (log_level == 0) {
            printf("来自 %s 请求文件 %s (Range: %ld-%ld)\n", inet_ntoa(client_addr->sin_addr), path, start, end);
            printf("Request from %s for file %s (Range: %ld-%ld)\n", inet_ntoa(client_addr->sin_addr), path, start, end);
        }
        send_file_range(client, fullpath, start, end);
    } else {
        if (log_level == 0) {
            printf("来自 %s 请求文件 %s\n", inet_ntoa(client_addr->sin_addr), path);
            printf("Request from %s for file %s\n", inet_ntoa(client_addr->sin_addr), path);
        }
        send_file_complete(client, fullpath);
    }
}

// 程序入口，解析命令行参数并启动 HTTP 服务
int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i+1 < argc) {
            root_dir = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i+1 < argc) {
            if (strcmp(argv[++i], "error") == 0) {
                log_level = 1;
            }
        } else {
            return 1;
        }
    }

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

    printf("HTTP 服务器已启动，端口 %d，根目录 %s\n", port, root_dir);
    printf("HTTP server started on port %d, serving %s\n", port, root_dir);

    fd_set readfds;
    struct timeval tv;
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ret = select(sock+1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret == 0) {
            continue;
        }

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // 处理请求，传入客户端地址用于日志
        handle_request(client, &client_addr);
        close(client);
    }
    close(sock);
    return 0;
}
