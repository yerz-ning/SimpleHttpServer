#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define BUFFER_SIZE 4096
#define DEFAULT_PORT 80

char *root_dir = ".";
int port = DEFAULT_PORT;

void send_response(int client, int code, char *status, char *type, char *body, size_t len) {
    char header[512];
    sprintf(header, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code, status, type, len);
    send(client, header, strlen(header), 0);
    if (body) send(client, body, len, 0);
}

void send_file(int client, char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        send_response(client, 404, "Not Found", "text/html", "<h1>404 Not Found</h1>", strlen("<h1>404 Not Found</h1>"));
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc(size);
    fread(buf, 1, size, f);
    fclose(f);

    char *ext = strrchr(path, '.');
    char *type = "application/octet-stream";
    if (ext) {
        if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) type = "text/html";
        else if (strcmp(ext, ".mp4") == 0) type = "video/mp4";
        else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) type = "image/jpeg";
        else if (strcmp(ext, ".png") == 0) type = "image/png";
        else if (strcmp(ext, ".css") == 0) type = "text/css";
        else if (strcmp(ext, ".js") == 0) type = "application/javascript";
    }
    send_response(client, 200, "OK", type, buf, size);
    free(buf);
}

void list_directory(int client, char *path) {
    DIR *d = opendir(path);
    if (!d) {
        send_response(client, 404, "Not Found", "text/html", "<h1>404 Not Found</h1>", strlen("<h1>404 Not Found</h1>"));
        return;
    }

    char *html = malloc(BUFFER_SIZE * 8);
    strcpy(html, "<!DOCTYPE html><html><head><title>File list</title></head><body><h2>Index of /</h2><ul>");
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        char fullpath[512];
        sprintf(fullpath, "%s/%s", path, entry->d_name);
        struct stat st;
        stat(fullpath, &st);
        if (S_ISDIR(st.st_mode)) {
            strcat(html, "<li><a href=\"");
            strcat(html, entry->d_name);
            strcat(html, "/\">");
            strcat(html, entry->d_name);
            strcat(html, "/</a></li>");
        } else {
            strcat(html, "<li><a href=\"");
            strcat(html, entry->d_name);
            strcat(html, "\">");
            strcat(html, entry->d_name);
            strcat(html, "</a></li>");
        }
    }
    strcat(html, "</ul></body></html>");
    closedir(d);
    send_response(client, 200, "OK", "text/html", html, strlen(html));
    free(html);
}

void handle_request(int client) {
    char req[1024];
    recv(client, req, sizeof(req)-1, 0);
    char *method = strtok(req, " ");
    char *path = strtok(NULL, " ");
    if (!method || !path) return;

    // 构造完整路径
    char fullpath[1024];
    if (strcmp(path, "/") == 0) {
        strcpy(fullpath, root_dir);
    } else {
        sprintf(fullpath, "%s%s", root_dir, path);
    }

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
        list_directory(client, fullpath);
    } else {
        send_file(client, fullpath);
    }
}

int main(int argc, char *argv[]) {
    // 解析参数
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
    if (listen(sock, 5) < 0) { perror("listen"); return 1; }

    printf("HTTP server running on port %d, serving %s\n", port, root_dir);
    while (1) {
        int client = accept(sock, NULL, NULL);
        if (client < 0) continue;
        handle_request(client);
        close(client);
    }
    return 0;
}
