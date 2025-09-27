#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <chrono>

// プラットフォーム固有のインクルード
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    #define close closesocket
    #define PATH_SEPARATOR "\\"
#else
    #include <sys/socket.h>
    #include <sys/wait.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <dirent.h>
    #include <errno.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define PATH_SEPARATOR "/"
    #define EINTR 4
#endif

// 関数プロトタイプ
void cleanup_socket();



bool is_cgi_file(const std::string& file_path) {
    // 拡張子をチェック
    std::string ext = "";
    size_t dot_pos = file_path.find_last_of('.');
    if (dot_pos != std::string::npos) {
        ext = file_path.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    
    // 一般的なCGI拡張子
    if (ext == ".cgi" || ext == ".pl" || ext == ".py" || ext == ".sh" || ext == ".rb") {
        return true;
    }
    
    // shebangがあるかチェック
    std::ifstream file(file_path);
    if (!file) return false;
    
    std::string first_line;
    std::getline(file, first_line);
    
    // shebang (#!) で始まるかチェック
    if (first_line.length() >= 2 && first_line.substr(0, 2) == "#!") {
        return true;
    }
    
#ifndef _WIN32
    // Linux/Unixの場合、実行権限もチェック
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) == 0) {
        if (file_stat.st_mode & S_IXUSR) {  // 実行権限がある
            return true;
        }
    }
#endif
    
    return false;
}

// グローバル変数でサーバーの状態を管理
std::atomic<bool> server_running(true);
SOCKET global_server_socket = INVALID_SOCKET;

// シグナルハンドラー
void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n\nサーバーを停止しています..." << std::endl;
        server_running = false;
        
        // サーバーソケットを閉じる
        if (global_server_socket != INVALID_SOCKET) {
            close(global_server_socket);
            global_server_socket = INVALID_SOCKET;
        }
        
        cleanup_socket();
        std::cout << "サーバーが正常に停止しました。" << std::endl;
        exit(0);
    }
}

struct Config {
    std::string root_dir;
    int port;
    std::string host;
    
    Config() : root_dir("."), port(8080), host("127.0.0.1") {}
};

// HTTPレスポンスの構造体
struct HttpResponse {
    int status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
};

// HTTPリクエストの構造体
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
};

// 文字列をトリム
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(' ');
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, (last - first + 1));
}

// 文字列を分割
std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// URLデコード
std::string url_decode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int hex_value;
            std::stringstream ss;
            ss << std::hex << str.substr(i + 1, 2);
            ss >> hex_value;
            result += static_cast<char>(hex_value);
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

// ファイルの存在確認
bool file_exists(const std::string& path) {
#ifdef _WIN32
    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
#endif
}

// ディレクトリの存在確認
bool dir_exists(const std::string& path) {
#ifdef _WIN32
    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode));
#endif
}

// ファイル読み込み
std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0);
    
    std::string content(size, '\0');
    file.read(&content[0], size);
    return content;
}

// MIMEタイプの取得
std::string get_mime_type(const std::string& path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos) return "application/octet-stream";
    
    std::string ext = path.substr(dot_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".txt") return "text/plain";
    if (ext == ".xml") return "application/xml";
    
    return "application/octet-stream";
}

// HTTPリクエストのパース
HttpRequest parse_http_request(const std::string& raw_request) {
    HttpRequest request;
    std::istringstream stream(raw_request);
    std::string line;
    
    // リクエストラインをパース
    if (std::getline(stream, line)) {
        line.pop_back(); // \r を削除
        std::vector<std::string> parts = split(line, ' ');
        if (parts.size() >= 3) {
            request.method = parts[0];
            request.path = parts[1];
            request.version = parts[2];
        }
    }
    
    // ヘッダーをパース
    while (std::getline(stream, line) && line != "\r") {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = trim(line.substr(0, colon_pos));
            std::string value = trim(line.substr(colon_pos + 1));
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
            request.headers[key] = value;
        }
    }
    
    // ボディを読み取り（POST リクエスト用）
    std::string body_content;
    while (std::getline(stream, line)) {
        body_content += line + "\n";
    }
    if (!body_content.empty()) {
        body_content.pop_back(); // 最後の改行を削除
    }
    request.body = body_content;
    
    return request;
}

// HTTPレスポンスの作成
std::string create_http_response(const HttpResponse& response) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response.status_code << " " << response.status_text << "\r\n";
    
    for (const auto& header : response.headers) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    
    oss << "\r\n" << response.body;
    return oss.str();
}

// shebangを読み取ってインタープリターを取得
std::string get_interpreter_from_shebang(const std::string& script_path) {
    std::ifstream file(script_path);
    if (!file) return "";
    
    std::string first_line;
    std::getline(file, first_line);
    
    // shebang (#!) で始まるかチェック
    if (first_line.length() >= 2 && first_line.substr(0, 2) == "#!") {
        std::string interpreter = first_line.substr(2);
        
        // 改行文字を削除
        if (!interpreter.empty() && interpreter.back() == '\r') {
            interpreter.pop_back();
        }
        
        // 先頭の空白を削除
        interpreter = trim(interpreter);
        
        // 引数が含まれている場合は最初の部分のみを取得
        size_t space_pos = interpreter.find(' ');
        if (space_pos != std::string::npos) {
            interpreter = interpreter.substr(0, space_pos);
        }
        
        return interpreter;
    }
    
    return "";
}

// CGI実行
std::string execute_cgi(const std::string& script_path, const HttpRequest& request, const std::string& root_dir) {
    std::string result = "";
    
    // shebangからインタープリターを取得
    std::string interpreter = get_interpreter_from_shebang(script_path);
    
#ifdef _WIN32
    // Windows での CGI 実行
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE hInputRead, hInputWrite, hOutputRead, hOutputWrite;
    SECURITY_ATTRIBUTES sa;
    
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    
    if (!CreatePipe(&hInputRead, &hInputWrite, &sa, 0) ||
        !CreatePipe(&hOutputRead, &hOutputWrite, &sa, 0)) {
        return "Status: 500 Internal Server Error\r\n\r\nCGI execution failed";
    }
    
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hOutputWrite;
    si.hStdOutput = hOutputWrite;
    si.hStdInput = hInputRead;
    si.dwFlags |= STARTF_USESTDHANDLES;
    
    // 環境変数の設定
    std::string env_vars = "REQUEST_METHOD=" + request.method + "\0";
    env_vars += "SCRIPT_NAME=" + request.path + "\0";
    env_vars += "CONTENT_LENGTH=" + std::to_string(request.body.length()) + "\0";
    env_vars += "QUERY_STRING=\0";  // 後で実装
    env_vars += "\0";
    
    // コマンドラインの構築
    std::string command_line;
    if (!interpreter.empty()) {
        // インタープリターが指定されている場合
        command_line = interpreter + " \"" + script_path + "\"";
    } else {
        // 直接実行の場合
        command_line = "\"" + script_path + "\"";
    }
    
    if (CreateProcessA(NULL, (LPSTR)command_line.c_str(), NULL, NULL, TRUE, 0, 
                      (LPVOID)env_vars.c_str(), root_dir.c_str(), &si, &pi)) {
        
        CloseHandle(hInputRead);
        CloseHandle(hOutputWrite);
        
        // 入力データを送信
        DWORD written;
        WriteFile(hInputWrite, request.body.c_str(), request.body.length(), &written, NULL);
        CloseHandle(hInputWrite);
        
        // 出力を読み取り
        char buffer[4096];
        DWORD read;
        while (ReadFile(hOutputRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0) {
            buffer[read] = '\0';
            result += buffer;
        }
        
        CloseHandle(hOutputRead);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        result = "Status: 500 Internal Server Error\r\n\r\nCGI execution failed";
    }
#else
    // Linux での CGI 実行
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return "Status: 500 Internal Server Error\r\n\r\nCGI execution failed";
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        return "Status: 500 Internal Server Error\r\n\r\nCGI execution failed";
    }
    
    if (pid == 0) {
        // 子プロセス
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        
        // 環境変数の設定
        setenv("REQUEST_METHOD", request.method.c_str(), 1);
        setenv("SCRIPT_NAME", request.path.c_str(), 1);
        setenv("CONTENT_LENGTH", std::to_string(request.body.length()).c_str(), 1);
        setenv("QUERY_STRING", "", 1);  // 後で実装
        
        // ディレクトリ変更（エラーチェック付き）
        if (chdir(root_dir.c_str()) != 0) {
            std::cerr << "Failed to change directory to: " << root_dir << std::endl;
            exit(1);
        }
        
        if (!interpreter.empty()) {
            // インタープリターが指定されている場合
            execl(interpreter.c_str(), interpreter.c_str(), script_path.c_str(), (char*)NULL);
        } else {
            // 直接実行の場合
            execl(script_path.c_str(), script_path.c_str(), (char*)NULL);
        }
        exit(1);
    } else {
        // 親プロセス
        close(pipefd[1]);
        
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            result += buffer;
        }
        
        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
    }
#endif
    
    return result;
}
// リクエスト処理
HttpResponse handle_request(const HttpRequest& request, const Config& config) {
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Server"] = "SimpleWebServer/1.0";
    
    std::string decoded_path = url_decode(request.path);
    std::string file_path = config.root_dir + PATH_SEPARATOR + decoded_path;
    
    // パスの正規化（セキュリティ対策）
    if (decoded_path.find("..") != std::string::npos) {
        response.status_code = 403;
        response.status_text = "Forbidden";
        response.headers["Content-Type"] = "text/html";
        response.body = "<html><body><h1>403 Forbidden</h1></body></html>";
        return response;
    }
    
    // ルートパスの場合はindex.htmlを探す
    if (decoded_path == "/" || decoded_path.empty()) {
        file_path = config.root_dir + PATH_SEPARATOR + "index.html";
    }
    
    // ファイルの存在確認
    if (file_exists(file_path)) {
        // CGI ファイルの処理
        if (is_cgi_file(file_path)) {
            std::string cgi_output = execute_cgi(file_path, request, config.root_dir);
            
            // CGI出力をパース
            size_t header_end = cgi_output.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                header_end = cgi_output.find("\n\n");
                if (header_end != std::string::npos) header_end += 1;
            } else {
                header_end += 2;
            }
            
            if (header_end != std::string::npos) {
                std::string headers = cgi_output.substr(0, header_end);
                std::string body = cgi_output.substr(header_end + 2);
                
                // ヘッダーをパース
                std::istringstream header_stream(headers);
                std::string line;
                while (std::getline(header_stream, line)) {
                    if (line.find("Status:") == 0) {
                        std::string status = line.substr(7);
                        size_t space_pos = status.find(' ');
                        if (space_pos != std::string::npos) {
                            response.status_code = std::stoi(trim(status.substr(0, space_pos)));
                            response.status_text = trim(status.substr(space_pos + 1));
                        }
                    } else if (line.find("Content-Type:") == 0) {
                        response.headers["Content-Type"] = trim(line.substr(13));
                    }
                }
                response.body = body;
            } else {
                response.body = cgi_output;
                if (response.headers.find("Content-Type") == response.headers.end()) {
                    response.headers["Content-Type"] = "text/html";
                }
            }
        } else {
            // 通常のファイル
            std::string content = read_file(file_path);
            if (content.empty()) {
                response.status_code = 500;
                response.status_text = "Internal Server Error";
                response.headers["Content-Type"] = "text/html";
                response.body = "<html><body><h1>500 Internal Server Error</h1></body></html>";
            } else {
                response.headers["Content-Type"] = get_mime_type(file_path);
                response.body = content;
            }
        }
    } else {
        response.status_code = 404;
        response.status_text = "Not Found";
        response.headers["Content-Type"] = "text/html";
        response.body = "<html><body><h1>404 Not Found</h1></body></html>";
    }
    
    response.headers["Content-Length"] = std::to_string(response.body.length());
    return response;
}

// クライアント処理
void handle_client(SOCKET client_socket, const Config& config) {
    if (!server_running) {
        close(client_socket);
        return;
    }
    
    char buffer[4096];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_received > 0 && server_running) {
        buffer[bytes_received] = '\0';
        
        HttpRequest request = parse_http_request(std::string(buffer));
        HttpResponse response = handle_request(request, config);
        std::string response_str = create_http_response(response);
        
        send(client_socket, response_str.c_str(), response_str.length(), 0);
    }
    
    close(client_socket);
}

// ソケット初期化
bool initialize_socket() {
#ifdef _WIN32
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
#else
    return true;
#endif
}

// ソケットクリーンアップ
void cleanup_socket() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// サーバー開始
void start_server(const Config& config) {
    // シグナルハンドラーの設定
    signal(SIGINT, signal_handler);
#ifndef _WIN32
    // Linuxでは追加のシグナルも処理
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
#endif
    
    if (!initialize_socket()) {
        std::cerr << "Failed to initialize socket" << std::endl;
        return;
    }
    
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        cleanup_socket();
        return;
    }
    
    global_server_socket = server_socket;
    
    // SO_REUSEADDR オプション
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config.port);
    server_addr.sin_addr.s_addr = inet_addr(config.host.c_str());
    
    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind socket" << std::endl;
        close(server_socket);
        cleanup_socket();
        return;
    }
    
    if (listen(server_socket, 10) == SOCKET_ERROR) {
        std::cerr << "Failed to listen on socket" << std::endl;
        close(server_socket);
        cleanup_socket();
        return;
    }
    
    std::cout << "Server starting on " << "http://" << config.host << ":" << config.port << std::endl;
    std::cout << "Document root: " << config.root_dir << std::endl;
    std::cout << "Press Ctrl+C to stop the server" << std::endl;
    std::cout << "======================================" << std::endl;
    
    while (server_running) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // タイムアウト付きでaccept
#ifdef _WIN32
        // Windowsでのノンブロッキング処理
        u_long mode = 1;
        ioctlsocket(server_socket, FIONBIO, &mode);
        
        SOCKET client_socket = accept(server_socket, (sockaddr*)&client_addr, &client_len);
        
        if (client_socket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                // ノンブロッキングでconnectionがない場合
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            } else {
                if (server_running) {
                    std::cerr << "Accept failed: " << error << std::endl;
                }
                break;
            }
        }
        
        // ブロッキングモードに戻す
        mode = 0;
        ioctlsocket(client_socket, FIONBIO, &mode);
#else
        // Linuxでのselect使用
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int select_result = select(server_socket + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) {
                // シグナルによる割り込み
                continue;
            }
            if (server_running) {
                std::cerr << "Select failed" << std::endl;
            }
            break;
        } else if (select_result == 0) {
            // タイムアウト
            continue;
        }
        
        SOCKET client_socket = accept(server_socket, (sockaddr*)&client_addr, &client_len);
        
        if (client_socket == INVALID_SOCKET) {
            if (server_running) {
                std::cerr << "Accept failed" << std::endl;
            }
            continue;
        }
#endif
        
        if (!server_running) {
            close(client_socket);
            break;
        }
        
        // 新しいスレッドでクライアントを処理
        std::thread client_thread(handle_client, client_socket, config);
        client_thread.detach();
    }
    
    close(server_socket);
    global_server_socket = INVALID_SOCKET;
    cleanup_socket();
    std::cout << "サーバーが停止しました。" << std::endl;
}

// ヘルプ表示
void show_help() {
    std::cout << "Simple Web Server v1.0" << std::endl;
    std::cout << "Usage: webserver [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -r, --root_dir DIR    Set document root directory (default: current directory)" << std::endl;
    std::cout << "  -p, --port PORT       Set port number (default: 8080)" << std::endl;
    std::cout << "  -h, --host HOST       Set host address (default: 127.0.0.1)" << std::endl;
    std::cout << "  -v, --version         Show version information" << std::endl;
    std::cout << "  --help               Show this help message" << std::endl;
}

// バージョン表示
void show_version() {
    std::cout << "Simple Web Server v1.0" << std::endl;
    std::cout << "Built with C++11" << std::endl;
}

// メイン関数
int main(int argc, char* argv[]) {
    Config config;
    
    // コマンドライン引数の解析
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            show_help();
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            show_version();
            return 0;
        } else if ((arg == "-r" || arg == "--root_dir") && i + 1 < argc) {
            config.root_dir = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.port = std::atoi(argv[++i]);
        } else if ((arg == "-h" || arg == "--host") && i + 1 < argc) {
            config.host = argv[++i];
        }
    }
    
    // ルートディレクトリの存在確認
    if (!dir_exists(config.root_dir)) {
        std::cerr << "Error: Root directory '" << config.root_dir << "' does not exist" << std::endl;
        return 1;
    }
    
    start_server(config);
    return 0;
}