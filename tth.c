#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>

#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define PATH_SEPARATOR "/"
#define BUFFER_SIZE 4096
#define MAX_HEADERS 50
#define MAX_PATH_LEN 2048
#define MAX_HEADER_LEN 1024

volatile sig_atomic_t server_running = 1;
int global_server_socket = INVALID_SOCKET;

typedef struct {
    char root_dir[MAX_PATH_LEN];
    int port;
    char host[256];
} Config;

typedef struct {
    int status_code;
    char status_text[64];
    char headers[MAX_HEADERS][MAX_HEADER_LEN];
    int header_count;
    char *body;
    size_t body_length;
} HttpResponse;

typedef struct {
    char method[16];
    char path[MAX_PATH_LEN];
    char version[16];
    char headers[MAX_HEADERS][MAX_HEADER_LEN];
    int header_count;
    char *body;
    size_t body_length;
} HttpRequest;

typedef struct {
    int client_socket;
    Config config;
    char client_ip[INET_ADDRSTRLEN];
} ClientData;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        printf("\n\nサーバーを停止しています...\n");
        server_running = 0;

        if (global_server_socket != INVALID_SOCKET) {
            close(global_server_socket);
            global_server_socket = INVALID_SOCKET;
        }

        printf("サーバーが正常に停止しました。\n");
        exit(0);
    }
}

char *trim(char *str) {
    char *end;

    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) return str;

    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    end[1] = '\0';
    return str;
}

int is_cgi_file(const char *file_path) {
    char *ext = strrchr(file_path, '.');
    if (ext) {
        if (strcasecmp(ext, ".cgi") == 0 || strcasecmp(ext, ".pl") == 0 ||
            strcasecmp(ext, ".py") == 0 || strcasecmp(ext, ".sh") == 0 ||
            strcasecmp(ext, ".rb") == 0) {
            return 1;
        }
    }

    FILE *file = fopen(file_path, "r");
    if (!file) return 0;

    char first_line[256];
    if (fgets(first_line, sizeof(first_line), file)) {
        fclose(file);
        return (strncmp(first_line, "#!", 2) == 0);
    }

    fclose(file);
    return 0;
}

int file_exists(const char *path) {
    struct stat buffer;
    return (stat(path, &buffer) == 0 && S_ISREG(buffer.st_mode));
}

int dir_exists(const char *path) {
    struct stat buffer;
    return (stat(path, &buffer) == 0 && S_ISDIR(buffer.st_mode));
}

void url_decode(const char *src, char *dest, size_t dest_size) {
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 1) {
        if (src[i] == '%' && i + 2 < strlen(src)) {
            int hex_value;
            if (sscanf(&src[i + 1], "%2x", &hex_value) == 1) {
                dest[j++] = (char)hex_value;
                i += 3;
            } else {
                dest[j++] = src[i++];
            }
        } else if (src[i] == '+') {
            dest[j++] = ' ';
            i++;
        } else {
            dest[j++] = src[i++];
        }
    }
    dest[j] = '\0';
}

const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0)
        return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    if (strcasecmp(ext, ".xml") == 0) return "application/xml";

    return "application/octet-stream";
}

char *read_file(const char *path, size_t *file_size) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = malloc(size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(content, 1, size, file);
    content[read_size] = '\0';
    fclose(file);

    if (file_size) *file_size = read_size;
    return content;
}

void parse_path_and_query(const char *path, char *clean_path, char *query_string) {
    const char *query_pos = strchr(path, '?');
    if (query_pos) {
        size_t path_len = query_pos - path;
        strncpy(clean_path, path, path_len);
        clean_path[path_len] = '\0';
        strcpy(query_string, query_pos + 1);
    } else {
        strcpy(clean_path, path);
        query_string[0] = '\0';
    }
}

void parse_script_and_path_info(const char *path, const char *root_dir, char *script_name, char *path_info) {
    strcpy(script_name, path);
    path_info[0] = '\0';

    char *token = strtok(strdup(path), "/");
    char current_path[MAX_PATH_LEN] = "";

    while (token) {
        strcat(current_path, "/");
        strcat(current_path, token);

        char file_path[MAX_PATH_LEN];
        snprintf(file_path, sizeof(file_path), "%s%s", root_dir, current_path);

        if (file_exists(file_path)) {
            strcpy(script_name, current_path);

            token = strtok(NULL, "/");
            if (token) {
                strcpy(path_info, "/");
                strcat(path_info, token);
                while ((token = strtok(NULL, "/"))) {
                    strcat(path_info, "/");
                    strcat(path_info, token);
                }
            }
            break;
        }
        token = strtok(NULL, "/");
    }
}

char *get_interpreter_from_shebang(const char *script_path) {
    FILE *file = fopen(script_path, "r");
    if (!file) return NULL;

    char first_line[256];
    if (!fgets(first_line, sizeof(first_line), file)) {
        fclose(file);
        return NULL;
    }
    fclose(file);

    if (strncmp(first_line, "#!", 2) != 0) return NULL;

    char *interpreter = strdup(first_line + 2);
    char *trimmed = trim(interpreter);

    char *space = strchr(trimmed, ' ');
    if (space) *space = '\0';

    char *newline = strchr(trimmed, '\n');
    if (newline) *newline = '\0';

    char *result = strdup(trimmed);
    free(interpreter);
    return result;
}

char *execute_cgi(const char *script_path, const HttpRequest *request, const char *root_dir, const Config *config, const char *client_ip) {
    char clean_path[MAX_PATH_LEN], query_string[MAX_PATH_LEN];
    parse_path_and_query(request->path, clean_path, query_string);

    char script_name[MAX_PATH_LEN], path_info[MAX_PATH_LEN];
    parse_script_and_path_info(clean_path, root_dir, script_name, path_info);

    char *interpreter = get_interpreter_from_shebang(script_path);

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        if (interpreter) free(interpreter);
        return strdup("Status: 500 Internal Server Error\r\n\r\nCGI execution failed");
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (interpreter) free(interpreter);
        return strdup("Status: 500 Internal Server Error\r\n\r\nCGI execution failed");
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        setenv("REQUEST_METHOD", request->method, 1);
        setenv("SCRIPT_NAME", script_name, 1);
        setenv("PATH_INFO", path_info, 1);

        char path_translated[MAX_PATH_LEN];
        snprintf(path_translated, sizeof(path_translated), "%s%s", root_dir, path_info);
        setenv("PATH_TRANSLATED", path_translated, 1);
        setenv("QUERY_STRING", query_string, 1);

        char content_length[32];
        snprintf(content_length, sizeof(content_length), "%zu", request->body_length);
        setenv("CONTENT_LENGTH", content_length, 1);

        for (int i = 0; i < request->header_count; i++) {
            char header_copy[MAX_HEADER_LEN];
            strcpy(header_copy, request->headers[i]);
            char *colon = strchr(header_copy, ':');
            if (colon) {
                *colon = '\0';
                char *key = header_copy;
                char *value = trim(colon + 1);

                if (strcasecmp(key, "Content-Type") == 0) {
                    setenv("CONTENT_TYPE", value, 1);
                }
            }
        }

        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_NAME", config->host, 1);

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", config->port);
        setenv("SERVER_PORT", port_str, 1);
        setenv("SERVER_PROTOCOL", request->version, 1);
        setenv("SERVER_SOFTWARE", "tth/1.0", 1);
        setenv("REMOTE_ADDR", client_ip, 1);
        setenv("REMOTE_HOST", client_ip, 1);

        if (chdir(root_dir) != 0) {
            fprintf(stderr, "Failed to change directory to: %s\n", root_dir);
            exit(1);
        }

        if (interpreter && strlen(interpreter) > 0) {
            execl(interpreter, interpreter, script_path, (char *)NULL);
        } else {
            execl(script_path, script_path, (char *)NULL);
        }
        exit(1);
    } else {
        close(pipefd[1]);

        char *result = malloc(BUFFER_SIZE);
        size_t result_size = 0;
        size_t result_capacity = BUFFER_SIZE;
        result[0] = '\0';

        char buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';

            if (result_size + bytes_read >= result_capacity) {
                result_capacity *= 2;
                result = realloc(result, result_capacity);
            }

            strcat(result, buffer);
            result_size += bytes_read;
        }

        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);

        if (interpreter) free(interpreter);
        return result;
    }
}

void log_request(const char *path, int status_code, const char *status_text) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    const char *color;
    if (status_code >= 200 && status_code < 300) {
        color = "\033[32m";
    } else if (status_code >= 300 && status_code < 400) {
        color = "\033[33m";
    } else if (status_code >= 400 && status_code < 500) {
        color = "\033[31m";
    } else if (status_code >= 500) {
        color = "\033[35m";
    } else {
        color = "\033[0m";
    }

    printf("\033[36m%04d-%02d-%02d %02d:%02d:%02d\033[0m %s%d\033[0m \033[34m%s\033[0m %s%s\033[0m\n",
           tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
           tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
           color, status_code, path, color, status_text);
}

void parse_http_request(const char *raw_request, HttpRequest *request) {
    memset(request, 0, sizeof(HttpRequest));

    char *request_copy = strdup(raw_request);
    char *line = strtok(request_copy, "\r\n");

    if (line) {
        char method[16], path[MAX_PATH_LEN], version[16];
        if (sscanf(line, "%15s %1023s %15s", method, path, version) == 3) {
            strcpy(request->method, method);
            strcpy(request->path, path);
            strcpy(request->version, version);
        }
    }

    while ((line = strtok(NULL, "\r\n")) && strlen(line) > 0) {
        if (request->header_count < MAX_HEADERS) {
            strcpy(request->headers[request->header_count], line);
            request->header_count++;
        }
    }

    line = strtok(NULL, "");
    if (line) {
        request->body_length = strlen(line);
        request->body = strdup(line);
    }

    free(request_copy);
}

char *create_http_response(const HttpResponse *response) {
    size_t response_size = 1024 + response->body_length;
    char *response_str = malloc(response_size);

    int offset = snprintf(response_str, response_size,
                         "HTTP/1.1 %d %s\r\n", response->status_code, response->status_text);

    for (int i = 0; i < response->header_count; i++) {
        offset += snprintf(response_str + offset, response_size - offset, "%s\r\n", response->headers[i]);
    }

    offset += snprintf(response_str + offset, response_size - offset, "\r\n");

    if (response->body && response->body_length > 0) {
        memcpy(response_str + offset, response->body, response->body_length);
        offset += response->body_length;
    }

    return response_str;
}

HttpResponse handle_request(const HttpRequest *request, const Config *config, const char *client_ip) {
    HttpResponse response;
    memset(&response, 0, sizeof(response));

    response.status_code = 200;
    strcpy(response.status_text, "OK");
    strcpy(response.headers[response.header_count++], "Server: tth/1.0");

    char decoded_path[MAX_PATH_LEN];
    url_decode(request->path, decoded_path, sizeof(decoded_path));

    if (strstr(decoded_path, "..")) {
        response.status_code = 403;
        strcpy(response.status_text, "Forbidden");
        strcpy(response.headers[response.header_count++], "Content-Type: text/html");
        response.body = strdup("<html><body><h1>403 Forbidden</h1></body></html>");
        response.body_length = strlen(response.body);

        char content_length[64];
        snprintf(content_length, sizeof(content_length), "Content-Length: %zu", response.body_length);
        strcpy(response.headers[response.header_count++], content_length);

        log_request(request->path, response.status_code, response.status_text);
        return response;
    }

    char file_path[MAX_PATH_LEN];
    if (strcmp(decoded_path, "/") == 0 || strlen(decoded_path) == 0) {
        if (strlen(config->root_dir) + 12 < sizeof(file_path)) {
            int ret = snprintf(file_path, sizeof(file_path), "%s/index.html", config->root_dir);
            (void)ret;  // Suppress unused variable warning
        } else {
            strcpy(file_path, "/index.html");
        }
    } else {
        if (strlen(config->root_dir) + strlen(decoded_path) + 1 < sizeof(file_path)) {
            int ret = snprintf(file_path, sizeof(file_path), "%s%s", config->root_dir, decoded_path);
            (void)ret;  // Suppress unused variable warning
        } else {
            strncpy(file_path, decoded_path, sizeof(file_path) - 1);
            file_path[sizeof(file_path) - 1] = '\0';
        }
    }

    if (file_exists(file_path)) {
        if (is_cgi_file(file_path)) {
            char *cgi_output = execute_cgi(file_path, request, config->root_dir, config, client_ip);

            char *header_end = strstr(cgi_output, "\r\n\r\n");
            if (!header_end) {
                header_end = strstr(cgi_output, "\n\n");
                if (header_end) header_end += 1;
            } else {
                header_end += 2;
            }

            if (header_end) {
                *header_end = '\0';
                char *body = header_end + 2;

                char *line = strtok(cgi_output, "\r\n");
                while (line) {
                    if (strncmp(line, "Status:", 7) == 0) {
                        int status_code;
                        char status_text[64];
                        if (sscanf(line + 7, "%d %63s", &status_code, status_text) >= 1) {
                            response.status_code = status_code;
                            if (strlen(status_text) > 0) {
                                strcpy(response.status_text, status_text);
                            }
                        }
                    } else if (strncmp(line, "Content-Type:", 13) == 0) {
                        char header[MAX_HEADER_LEN];
                        snprintf(header, sizeof(header), "Content-Type: %s", trim(line + 13));
                        strcpy(response.headers[response.header_count++], header);
                    }
                    line = strtok(NULL, "\r\n");
                }

                response.body = strdup(body);
                response.body_length = strlen(body);
            } else {
                response.body = cgi_output;
                response.body_length = strlen(cgi_output);
                strcpy(response.headers[response.header_count++], "Content-Type: text/html");
            }
        } else {
            size_t file_size;
            char *content = read_file(file_path, &file_size);
            if (!content) {
                response.status_code = 500;
                strcpy(response.status_text, "Internal Server Error");
                strcpy(response.headers[response.header_count++], "Content-Type: text/html");
                response.body = strdup("<html><body><h1>500 Internal Server Error</h1></body></html>");
                response.body_length = strlen(response.body);
            } else {
                char content_type[128];
                snprintf(content_type, sizeof(content_type), "Content-Type: %s", get_mime_type(file_path));
                strcpy(response.headers[response.header_count++], content_type);
                response.body = content;
                response.body_length = file_size;
            }
        }
    } else {
        response.status_code = 404;
        strcpy(response.status_text, "Not Found");
        strcpy(response.headers[response.header_count++], "Content-Type: text/html");
        response.body = strdup("<html><body><h1>404 Not Found</h1></body></html>");
        response.body_length = strlen(response.body);
    }

    char content_length[64];
    snprintf(content_length, sizeof(content_length), "Content-Length: %zu", response.body_length);
    strcpy(response.headers[response.header_count++], content_length);

    log_request(request->path, response.status_code, response.status_text);
    return response;
}

void *handle_client(void *arg) {
    ClientData *data = (ClientData *)arg;

    if (!server_running) {
        close(data->client_socket);
        free(data);
        return NULL;
    }

    char buffer[BUFFER_SIZE];
    int bytes_received = recv(data->client_socket, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received > 0 && server_running) {
        buffer[bytes_received] = '\0';

        HttpRequest request;
        parse_http_request(buffer, &request);

        HttpResponse response = handle_request(&request, &data->config, data->client_ip);
        char *response_str = create_http_response(&response);

        send(data->client_socket, response_str, strlen(response_str), 0);

        free(response_str);
        if (response.body) free(response.body);
        if (request.body) free(request.body);
    }

    close(data->client_socket);
    free(data);
    return NULL;
}

void start_server(const Config *config) {
    signal(SIGINT, signal_handler);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        fprintf(stderr, "Failed to create socket\n");
        return;
    }

    global_server_socket = server_socket;

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config->port);
    server_addr.sin_addr.s_addr = inet_addr(config->host);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Failed to bind socket\n");
        close(server_socket);
        return;
    }

    if (listen(server_socket, 10) == SOCKET_ERROR) {
        fprintf(stderr, "Failed to listen on socket\n");
        close(server_socket);
        return;
    }

    printf("Server starting on http://%s:%d\n", config->host, config->port);
    printf("Document root: %s\n", config->root_dir);
    printf("Press Ctrl+C to stop the server\n");
    printf("======================================\n");

    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int select_result = select(server_socket + 1, &read_fds, NULL, NULL, &timeout);

        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (server_running) {
                fprintf(stderr, "Select failed\n");
            }
            break;
        } else if (select_result == 0) {
            continue;
        }

        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

        if (client_socket == INVALID_SOCKET) {
            if (server_running) {
                fprintf(stderr, "Accept failed\n");
            }
            continue;
        }

        if (!server_running) {
            close(client_socket);
            break;
        }

        ClientData *data = malloc(sizeof(ClientData));
        data->client_socket = client_socket;
        data->config = *config;
        strcpy(data->client_ip, inet_ntoa(client_addr.sin_addr));

        pthread_t client_thread;
        pthread_create(&client_thread, NULL, handle_client, data);
        pthread_detach(client_thread);
    }

    close(server_socket);
    global_server_socket = INVALID_SOCKET;
    printf("サーバーが停止しました。\n");
}

void show_help() {
    printf("tth - tiny tiny httpd v1.0\n");
    printf("Usage: tth [options]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -r, --root_dir DIR    Set document root directory (default: current directory)\n");
    printf("  -p, --port PORT       Set port number (default: 8080)\n");
    printf("  -h, --host HOST       Set host address (default: 127.0.0.1)\n");
    printf("  -v, --version         Show version information\n");
    printf("  --help               Show this help message\n");
}

void show_version() {
    printf("tth - tiny tiny httpd v1.0\n");
    printf("Built with C\n");
}

int main(int argc, char *argv[]) {
    Config config;
    strcpy(config.root_dir, ".");
    config.port = 8080;
    strcpy(config.host, "127.0.0.1");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            show_help();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            show_version();
            return 0;
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--root_dir") == 0) && i + 1 < argc) {
            strcpy(config.root_dir, argv[++i]);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            config.port = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            strcpy(config.host, argv[++i]);
        }
    }

    if (!dir_exists(config.root_dir)) {
        fprintf(stderr, "Error: Root directory '%s' does not exist\n", config.root_dir);
        return 1;
    }

    start_server(&config);
    return 0;
}