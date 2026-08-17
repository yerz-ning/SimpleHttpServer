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
int log_level = 1;

// 将缓冲区数据全部发送到套接字描述符，失败返回 -1
ssize_t send_all(int fd, const void *buf, size_t len) {
    ssize_t total = 0;
    while (total < (ssize_t)len) {
        ssize_t n = send(fd, (char*)buf + total, len - total, 0);
        if (n <= 0) {
            if (log_level == 0) {
                printf("发送失败，已发送 %ld 字节\n", total);
                printf("Send failed, sent %ld bytes\n", total);
            }
            return -1;
        }
        total += n;
        if (log_level == 0 && n > 0) {
            printf("发送 %ld 字节，累计 %ld 字节\n", n, total);
            printf("Sent %ld bytes, total %ld bytes\n", n, total);
        }
    }
    return total;
}

// 向客户端发送 HTTP 响应头，失败返回 -1
int send_header(int client, int code, const char *status, const char *type,
                long content_length, const char *extra) {
    if (log_level == 0) {
        printf("准备发送响应头：状态码 %d，内容类型 %s，长度 %ld\n", code, type, content_length);
        printf("Preparing response header: status %d, type %s, length %ld\n", code, type, content_length);
    }
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        code, status, type, content_length, extra ? extra : "");
    if (send_all(client, header, len) < 0)
        return -1;
    if (log_level == 0) {
        printf("响应头发送完成\n");
        printf("Response header sent\n");
    }
    return 0;
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
    if (log_level == 0) {
        printf("打开文件 %s (Range: %ld-%ld)\n", path, start, end);
        printf("Opening file %s (Range: %ld-%ld)\n", path, start, end);
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (log_level == 0) {
            printf("打开文件失败 %s\n", path);
            printf("Failed to open file %s\n", path);
        }
        send_header(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    if (log_level == 0) {
        printf("文件打开成功 %s\n", path);
        printf("File opened successfully %s\n", path);
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        if (log_level == 0) {
            printf("获取文件状态失败 %s\n", path);
            printf("Failed to get file stats %s\n", path);
        }
        send_header(client, 500, "Internal Error", "text/html", 0, NULL);
        return;
    }

    long file_size = st.st_size;
    if (end == -1) end = file_size - 1;
    if (start > end || start >= file_size) {
        close(fd);
        if (log_level == 0) {
            printf("Range 请求无效: %ld-%ld，文件大小 %ld\n", start, end, file_size);
            printf("Invalid Range: %ld-%ld, file size %ld\n", start, end, file_size);
        }
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

    lseek(fd, start, SEEK_SET);
    char buf[BUFFER_SIZE];
    long remaining = content_length;
    int chunk_count = 0;
    while (remaining > 0) {
        long to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        ssize_t n = read(fd, buf, to_read);
        if (n <= 0) break;
        chunk_count++;
        if (log_level == 0) {
            printf("发送文件块 %d，大小 %ld 字节，剩余 %ld 字节\n", chunk_count, n, remaining - n);
            printf("Sending file chunk %d, size %ld bytes, remaining %ld bytes\n", chunk_count, n, remaining - n);
        }
        if (send_all(client, buf, n) < 0) break;
        remaining -= n;
    }
    close(fd);
    if (log_level == 0) {
        printf("文件发送完成 %s，共 %d 个块，实际发送 %ld 字节\n", path, chunk_count, content_length - remaining);
        printf("File send complete %s, %d chunks, sent %ld bytes\n", path, chunk_count, content_length - remaining);
    }
}

// 以流式方式发送完整的文件内容
void send_file_complete(int client, const char *path) {
    if (log_level == 0) {
        printf("打开文件 %s\n", path);
        printf("Opening file %s\n", path);
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (log_level == 0) {
            printf("打开文件失败 %s\n", path);
            printf("Failed to open file %s\n", path);
        }
        send_header(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    if (log_level == 0) {
        printf("文件打开成功 %s\n", path);
        printf("File opened successfully %s\n", path);
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        if (log_level == 0) {
            printf("获取文件状态失败 %s\n", path);
            printf("Failed to get file stats %s\n", path);
        }
        send_header(client, 500, "Internal Error", "text/html", 0, NULL);
        return;
    }

    if (send_header(client, 200, "OK", get_mime_type(path), st.st_size, NULL) < 0) {
        close(fd);
        return;
    }

    char buf[BUFFER_SIZE];
    ssize_t n;
    int chunk_count = 0;
    long total_sent = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        chunk_count++;
        if (log_level == 0) {
            printf("发送文件块 %d，大小 %ld 字节，累计 %ld 字节\n", chunk_count, n, total_sent + n);
            printf("Sending file chunk %d, size %ld bytes, total %ld bytes\n", chunk_count, n, total_sent + n);
        }
        if (send_all(client, buf, n) < 0) break;
        total_sent += n;
    }
    close(fd);
    if (log_level == 0) {
        printf("文件发送完成 %s，共 %d 个块，总大小 %ld 字节\n", path, chunk_count, total_sent);
        printf("File send complete %s, %d chunks, total %ld bytes\n", path, chunk_count, total_sent);
    }
}

// 流式生成指定目录的 HTML 文件列表
void list_directory(int client, const char *path) {
    if (log_level == 0) {
        printf("打开目录 %s\n", path);
        printf("Opening directory %s\n", path);
    }
    DIR *d = opendir(path);
    if (!d) {
        if (log_level == 0) {
            printf("打开目录失败 %s\n", path);
            printf("Failed to open directory %s\n", path);
        }
        send_header(client, 404, "Not Found", "text/html", 0, NULL);
        return;
    }
    if (log_level == 0) {
        printf("目录打开成功 %s\n", path);
        printf("Directory opened successfully %s\n", path);
    }

    const char *head = "<!DOCTYPE html><html><head><meta charset=utf-8><title>File list</title></head><body><h2>Index of ";
    if (send_all(client, head, strlen(head)) < 0) {
        closedir(d);
        return;
    }
    if (send_all(client, path, strlen(path)) < 0) {
        closedir(d);
        return;
    }
    const char *mid = "</h2><ul>";
    if (send_all(client, mid, strlen(mid)) < 0) {
        closedir(d);
        return;
    }

    struct dirent *entry;
    char fullpath[512];
    int entry_count = 0;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        entry_count++;
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        char line[512];
        if (S_ISDIR(st.st_mode)) {
            snprintf(line, sizeof(line), "<li><a href=\"%s/\">%s/</a></li>", entry->d_name, entry->d_name);
        } else {
            snprintf(line, sizeof(line), "<li><a href=\"%s\">%s</a></li>", entry->d_name, entry->d_name);
        }
        if (log_level == 0) {
            printf("目录条目 %d: %s (%s)\n", entry_count, entry->d_name, S_ISDIR(st.st_mode) ? "目录" : "文件");
            printf("Directory entry %d: %s (%s)\n", entry_count, entry->d_name, S_ISDIR(st.st_mode) ? "dir" : "file");
        }
        if (send_all(client, line, strlen(line)) < 0) {
            closedir(d);
            return;
        }
    }
    const char *tail = "</ul></body></html>";
    send_all(client, tail, strlen(tail));
    closedir(d);
    if (log_level == 0) {
        printf("目录列表发送完成 %s，共 %d 个条目\n", path, entry_count);
        printf("Directory list sent %s, %d entries\n", path, entry_count);
    }
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

// 处理单个 HTTP 请求
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

    if (log_level == 0) {
        printf("收到请求：方法 %s，路径 %s\n", method, path);
        printf("Received request: method %s, path %s\n", method, path);
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
    if (log_level == 0) {
        printf("请求头读取完成，共 %d 字节\n", total);
        printf("Headers read complete, %d bytes\n", total);
    }

    char fullpath[1024];
    if (strcmp(path, "/") == 0) {
        snprintf(fullpath, sizeof(fullpath), "%s", root_dir);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s%s", root_dir, path);
    }

    if (log_level == 0) {
        printf("完整路径: %s\n", fullpath);
        printf("Full path: %s\n", fullpath);
    }

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (log_level == 0) {
            printf("路径是目录，开始生成列表\n");
            printf("Path is directory, generating list\n");
        }
        list_directory(client, fullpath);
        if (log_level == 0) {
            printf("目录请求处理完成\n");
            printf("Directory request complete\n");
        }
        return;
    }

    long start = 0, end = -1;
    if (parse_range(headers, &start, &end)) {
        if (log_level == 0) {
            printf("Range 请求: %ld-%ld\n", start, end);
            printf("Range request: %ld-%ld\n", start, end);
        }
        send_file_range(client, fullpath, start, end);
    } else {
        send_file_complete(client, fullpath);
    }
    if (log_level == 0) {
        printf("文件请求处理完成\n");
        printf("File request complete\n");
    }
}

// 程序入口
int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i+1 < argc) {
            root_dir = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i+1 < argc) {
            if (strcmp(argv[++i], "info") == 0) {
                log_level = 0;
            }
        } else {
            return 1;
        }
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        printf("套接字创建失败\n");
        return 1;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        printf("绑定端口失败\n");
        return 1;
    }
    if (listen(sock, BACKLOG) < 0) {
        perror("listen");
        printf("监听失败\n");
        return 1;
    }

    printf("HTTP 服务器已启动，端口 %d，根目录 %s\n", port, root_dir);
    printf("HTTP server started on port %d, serving %s\n", port, root_dir);
    if (log_level == 0) {
        printf("日志级别：详细\n");
        printf("Log level: info\n");
    } else {
        printf("日志级别：错误\n");
        printf("Log level: error\n");
    }

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
            printf("select 错误\n");
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
            printf("接受连接失败\n");
            continue;
        }

        if (log_level == 0) {
            printf("新连接来自 %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            printf("New connection from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        }
        handle_request(client, &client_addr);
        close(client);
        if (log_level == 0) {
            printf("连接关闭 %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            printf("Connection closed %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        }
    }
    close(sock);
    return 0;
}
