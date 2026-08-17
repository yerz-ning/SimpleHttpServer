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

#define BUFFER_SIZE 4096
#define DEFAULT_PORT 80
#define BACKLOG 5

char *root_dir = ".";
int port = DEFAULT_PORT;

// 发送所有数据，失败返回 -1
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
void send_response(int client, int code, const char *status, const char *type,
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

// 获取 MIME 类型
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

// 流式发送文件（支持 Range）
void send_file_range(int client, const char *path, long start, long end) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_response(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        send_response(client, 500, "Internal Error", "text/html", 0, NULL);
        return;
    }
    long file_size = st.st_size;
    if (end == -1) end = file_size - 1;
    if (start > end || start >= file_size) {
        close(fd);
        send_response(client, 416, "Range Not Satisfiable", "text/html", 0, NULL);
        return;
    }
    if (end >= file_size) end = file_size - 1;
    long content_length = end - start + 1;
    char extra[128];
    snprintf(extra, sizeof(extra), "Content-Range: bytes %ld-%ld/%ld\r\n", start, end, file_size);
    send_response(client, 206, "Partial Content", get_mime_type(path), content_length, extra);
    lseek(fd, start, SEEK_SET);
    char buf[BUFFER_SIZE];
    long remaining = content_length;
    while (remaining > 0) {
        long to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        ssize_t n = read(fd, buf, to_read);
        if (n <= 0) break;
        if (send_all(client, buf, n) < 0) break;
        remaining -= n;
    }
    close(fd);
}

// 流式发送完整文件
void send_file_complete(int client, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_response(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        send_response(client, 500, "Internal Error", "text/html", 0, NULL);
        return;
    }
    send_response(client, 200, "OK", get_mime_type(path), st.st_size, NULL);
    char buf[BUFFER_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (send_all(client, buf, n) < 0) break;
    }
    close(fd);
}

// 流式生成目录列表
void list_directory(int client, const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        send_response(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    const char *head = "<!DOCTYPE html><html><head><meta charset=utf-8><title>File list</title></head><body><h2>Index of ";
    if (send_all(client, head, strlen(head)) < 0) { closedir(d); return; }
    if (send_all(client, path, strlen(path)) < 0) { closedir(d); return; }
    const char *mid = "</h2><ul>";
    if (send_all(client, mid, strlen(mid)) < 0) { closedir(d); return; }
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
        if (send_all(client, line, strlen(line)) < 0) { closedir(d); return; }
    }
    const char *tail = "</ul></body></html>";
    send_all(client, tail, strlen(tail));
    closedir(d);
}

// 处理请求（第一版风格）
void handle_request(int client) {
    char buf[BUFFER_SIZE];
    int n = recv(client, buf, sizeof(buf)-1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    char *method = strtok(buf, " ");
    char *path = strtok(NULL, " ");
    if (!method || !path) return;

    // 查找 Range 头
    long start = 0, end = -1;
    int has_range = 0;
    char *range_header = strstr(buf, "Range: bytes=");
    if (range_header) {
        range_header += 13;
        char *dash = strchr(range_header, '-');
        if (dash) {
            start = atol(range_header);
            if (*(dash+1) != '\0') end = atol(dash+1);
            else end = -1;
            has_range = 1;
        }
    }

    char fullpath[1024];
    if (strcmp(path, "/") == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s", root_dir);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s", root_dir, path);
    }

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
        list_directory(client, fullpath);
        return;
    }

    if (has_range) {
        send_file_range(client, fullpath, start, end);
    } else {
        send_file_complete(client, fullpath);
    }
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i+1 < argc) {
            root_dir = argv[++i];
        } else {
            printf("Usage: %s [-p port] [-r root_dir]\n", argv[0]);
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

    printf("HTTP server running on port %d, serving %s\n", port, root_dir);

    while (1) {
        int client = accept(sock, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_request(client);
        close(client);
    }
    close(sock);
    return 0;
}
