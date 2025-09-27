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
#include <iomanip>

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


bool is_cgi_file(const std::string &file_path)
{
    // 拡張子をチェック
    std::string ext = "";
    size_t dot_pos = file_path.find_last_of('.');
    if (dot_pos != std::string::npos)
    {
        ext = file_path.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    // 一般的なCGI拡張子
    if (ext == ".cgi" || ext == ".pl" || ext == ".py" || ext == ".sh" || ext == ".rb")
    {
        return true;
    }

    // shebangがあるかチェック
    std::ifstream file(file_path);
    if (!file)
        return false;

    std::string first_line;
    std::getline(file, first_line);

    // shebang (#!) で始まるかチェック
    if (first_line.length() >= 2 && first_line.substr(0, 2) == "#!")
    {
        return true;
    }

    return false;
}

// グローバル変数でサーバーの状態を管理
std::atomic<bool> server_running(true);
SOCKET global_server_socket = INVALID_SOCKET;

// シグナルハンドラー
void signal_handler(int signal)
{
    if (signal == SIGINT)
    {
        std::cout << "\n\nサーバーを停止しています..." << std::endl;
        server_running = false;

        // サーバーソケットを閉じる
        if (global_server_socket != INVALID_SOCKET)
        {
            close(global_server_socket);
            global_server_socket = INVALID_SOCKET;
        }

        std::cout << "サーバーが正常に停止しました。" << std::endl;
        exit(0);
    }
}

struct Config
{
    std::string root_dir;
    int port;
    std::string host;

    Config() : root_dir("."), port(8080), host("127.0.0.1") {}
};

// HTTPレスポンスの構造体
struct HttpResponse
{
    int status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
};

// HTTPリクエストの構造体
struct HttpRequest
{
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
};

// 文字列をトリム
std::string trim(const std::string &str)
{
    size_t first = str.find_first_not_of(' ');
    if (first == std::string::npos)
        return "";
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, (last - first + 1));
}

// 文字列を分割
std::vector<std::string> split(const std::string &str, char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}

// URLデコード
std::string url_decode(const std::string &str)
{
    std::string result;
    for (size_t i = 0; i < str.length(); ++i)
    {
        if (str[i] == '%' && i + 2 < str.length())
        {
            int hex_value;
            std::stringstream ss;
            ss << std::hex << str.substr(i + 1, 2);
            ss >> hex_value;
            result += static_cast<char>(hex_value);
            i += 2;
        }
        else if (str[i] == '+')
        {
            result += ' ';
        }
        else
        {
            result += str[i];
        }
    }
    return result;
}

// ファイルの存在確認
bool file_exists(const std::string &path)
{
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
}

// クエリストリングとパス情報を分離
std::pair<std::string, std::string> parse_path_and_query(const std::string &path)
{
    size_t query_pos = path.find('?');
    if (query_pos != std::string::npos)
    {
        return std::make_pair(path.substr(0, query_pos), path.substr(query_pos + 1));
    }
    return std::make_pair(path, "");
}

// PATH_INFOとSCRIPT_NAMEを分離
std::pair<std::string, std::string> parse_script_and_path_info(const std::string &path, const std::string &root_dir)
{
    std::string script_name = path;
    std::string path_info = "";

    // パスの各セグメントをチェックして、実際に存在するファイルを見つける
    std::vector<std::string> segments = split(path, '/');
    std::string current_path = "";

    for (size_t i = 0; i < segments.size(); ++i)
    {
        if (!segments[i].empty())
        {
            current_path += "/" + segments[i];
            std::string file_path = root_dir + current_path;

            if (file_exists(file_path))
            {
                script_name = current_path;
                // 残りのセグメントがPATH_INFO
                if (i + 1 < segments.size())
                {
                    for (size_t j = i + 1; j < segments.size(); ++j)
                    {
                        path_info += "/" + segments[j];
                    }
                }
                break;
            }
        }
    }

    return std::make_pair(script_name, path_info);
}

// ディレクトリの存在確認
bool dir_exists(const std::string &path)
{
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode));
}

// ファイル読み込み
std::string read_file(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return "";

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0);

    std::string content(size, '\0');
    file.read(&content[0], size);
    return content;
}

// MIMEタイプの取得
std::string get_mime_type(const std::string &path)
{
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos)
        return "application/octet-stream";

    std::string ext = path.substr(dot_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".html" || ext == ".htm")
        return "text/html";
    if (ext == ".css")
        return "text/css";
    if (ext == ".js")
        return "application/javascript";
    if (ext == ".json")
        return "application/json";
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".gif")
        return "image/gif";
    if (ext == ".txt")
        return "text/plain";
    if (ext == ".xml")
        return "application/xml";

    return "application/octet-stream";
}

// HTTPリクエストのパース
HttpRequest parse_http_request(const std::string &raw_request)
{
    HttpRequest request;
    std::istringstream stream(raw_request);
    std::string line;

    // リクエストラインをパース
    if (std::getline(stream, line))
    {
        line.pop_back(); // \r を削除
        std::vector<std::string> parts = split(line, ' ');
        if (parts.size() >= 3)
        {
            request.method = parts[0];
            request.path = parts[1];
            request.version = parts[2];
        }
    }

    // ヘッダーをパース
    while (std::getline(stream, line) && line != "\r")
    {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos)
        {
            std::string key = trim(line.substr(0, colon_pos));
            std::string value = trim(line.substr(colon_pos + 1));
            if (!value.empty() && value.back() == '\r')
            {
                value.pop_back();
            }
            request.headers[key] = value;
        }
    }

    // ボディを読み取り（POST リクエスト用）
    std::string body_content;
    while (std::getline(stream, line))
    {
        body_content += line + "\n";
    }
    if (!body_content.empty())
    {
        body_content.pop_back(); // 最後の改行を削除
    }
    request.body = body_content;

    return request;
}

// HTTPレスポンスの作成
std::string create_http_response(const HttpResponse &response)
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response.status_code << " " << response.status_text << "\r\n";

    for (const auto &header : response.headers)
    {
        oss << header.first << ": " << header.second << "\r\n";
    }

    oss << "\r\n"
        << response.body;
    return oss.str();
}

// shebangを読み取ってインタープリターを取得
std::string get_interpreter_from_shebang(const std::string &script_path)
{
    std::ifstream file(script_path);
    if (!file)
        return "";

    std::string first_line;
    std::getline(file, first_line);

    // shebang (#!) で始まるかチェック
    if (first_line.length() >= 2 && first_line.substr(0, 2) == "#!")
    {
        std::string interpreter = first_line.substr(2);

        // 改行文字を削除
        if (!interpreter.empty() && interpreter.back() == '\r')
        {
            interpreter.pop_back();
        }

        // 先頭の空白を削除
        interpreter = trim(interpreter);

        // 引数が含まれている場合は最初の部分のみを取得
        size_t space_pos = interpreter.find(' ');
        if (space_pos != std::string::npos)
        {
            interpreter = interpreter.substr(0, space_pos);
        }

        return interpreter;
    }

    return "";
}

// CGI実行
std::string execute_cgi(const std::string &script_path, const HttpRequest &request, const std::string &root_dir, const Config &config, const std::string &client_ip)
{
    std::string result = "";

    // パスとクエリストリングを分離
    std::pair<std::string, std::string> path_query = parse_path_and_query(request.path);
    std::string clean_path = path_query.first;
    std::string query_string = path_query.second;

    // SCRIPT_NAMEとPATH_INFOを分離
    std::pair<std::string, std::string> script_pathinfo = parse_script_and_path_info(clean_path, root_dir);
    std::string script_name = script_pathinfo.first;
    std::string path_info = script_pathinfo.second;

    // shebangからインタープリターを取得
    std::string interpreter = get_interpreter_from_shebang(script_path);

    // Linux での CGI 実行
    int pipefd[2];
    if (pipe(pipefd) == -1)
    {
        return "Status: 500 Internal Server Error\r\n\r\nCGI execution failed";
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        return "Status: 500 Internal Server Error\r\n\r\nCGI execution failed";
    }

    if (pid == 0)
    {
        // 子プロセス
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // 基本的なCGI環境変数
        setenv("REQUEST_METHOD", request.method.c_str(), 1);
        setenv("SCRIPT_NAME", script_name.c_str(), 1);
        setenv("PATH_INFO", path_info.c_str(), 1);

        std::string path_translated = root_dir + path_info;
        setenv("PATH_TRANSLATED", path_translated.c_str(), 1);
        setenv("QUERY_STRING", query_string.c_str(), 1);
        setenv("CONTENT_LENGTH", std::to_string(request.body.length()).c_str(), 1);

        // Content-Type
        auto content_type_it = request.headers.find("Content-Type");
        if (content_type_it != request.headers.end())
        {
            setenv("CONTENT_TYPE", content_type_it->second.c_str(), 1);
        }

        // サーバー情報
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_NAME", config.host.c_str(), 1);
        setenv("SERVER_PORT", std::to_string(config.port).c_str(), 1);
        setenv("SERVER_PROTOCOL", request.version.c_str(), 1);
        setenv("SERVER_SOFTWARE", "SimpleWebServer/1.0", 1);

        // クライアント情報
        setenv("REMOTE_ADDR", client_ip.c_str(), 1);
        setenv("REMOTE_HOST", client_ip.c_str(), 1); // 簡易実装

        // HTTPヘッダー
        for (const auto &header : request.headers)
        {
            std::string header_name = "HTTP_" + header.first;
            std::transform(header_name.begin(), header_name.end(), header_name.begin(), ::toupper);
            std::replace(header_name.begin(), header_name.end(), '-', '_');
            setenv(header_name.c_str(), header.second.c_str(), 1);
        }

        // ディレクトリ変更（エラーチェック付き）
        if (chdir(root_dir.c_str()) != 0)
        {
            std::cerr << "Failed to change directory to: " << root_dir << std::endl;
            exit(1);
        }

        if (!interpreter.empty())
        {
            // インタープリターが指定されている場合
            execl(interpreter.c_str(), interpreter.c_str(), script_path.c_str(), (char *)NULL);
        }
        else
        {
            // 直接実行の場合
            execl(script_path.c_str(), script_path.c_str(), (char *)NULL);
        }
        exit(1);
    }
    else
    {
        // 親プロセス
        close(pipefd[1]);

        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0)
        {
            buffer[bytes_read] = '\0';
            result += buffer;
        }

        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
    }

    return result;
}

// ログ出力関数
void log_request(const std::string &path, int status_code, const std::string &status_text)
{
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);

    // ステータスコードに応じて色を決定
    std::string color;
    if (status_code >= 200 && status_code < 300) {
        color = "\033[32m"; // 緑色 (成功)
    } else if (status_code >= 300 && status_code < 400) {
        color = "\033[33m"; // 黄色 (リダイレクト)
    } else if (status_code >= 400 && status_code < 500) {
        color = "\033[31m"; // 赤色 (クライアントエラー)
    } else if (status_code >= 500) {
        color = "\033[35m"; // マゼンタ (サーバーエラー)
    } else {
        color = "\033[0m";  // デフォルト色
    }

    std::cout << "\033[36m" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\033[0m"
              << " " << color << status_code << "\033[0m"
              << " \033[34m" << path << "\033[0m"
              << " " << color << status_text << "\033[0m"
              << std::endl;
}

// リクエスト処理
HttpResponse handle_request(const HttpRequest &request, const Config &config, const std::string &client_ip)
{
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Server"] = "tth/1.0";

    std::string decoded_path = url_decode(request.path);
    std::string file_path = config.root_dir + PATH_SEPARATOR + decoded_path;

    // パスの正規化（セキュリティ対策）
    if (decoded_path.find("..") != std::string::npos)
    {
        response.status_code = 403;
        response.status_text = "Forbidden";
        response.headers["Content-Type"] = "text/html";
        response.body = "<html><body><h1>403 Forbidden</h1></body></html>";
        return response;
    }

    // ルートパスの場合はindex.htmlを探す
    if (decoded_path == "/" || decoded_path.empty())
    {
        file_path = config.root_dir + PATH_SEPARATOR + "index.html";
    }

    // ファイルの存在確認
    if (file_exists(file_path))
    {
        // CGI ファイルの処理
        if (is_cgi_file(file_path))
        {
            std::string cgi_output = execute_cgi(file_path, request, config.root_dir, config, client_ip);

            // CGI出力をパース
            size_t header_end = cgi_output.find("\r\n\r\n");
            if (header_end == std::string::npos)
            {
                header_end = cgi_output.find("\n\n");
                if (header_end != std::string::npos)
                    header_end += 1;
            }
            else
            {
                header_end += 2;
            }

            if (header_end != std::string::npos)
            {
                std::string headers = cgi_output.substr(0, header_end);
                std::string body = cgi_output.substr(header_end + 2);

                // ヘッダーをパース
                std::istringstream header_stream(headers);
                std::string line;
                while (std::getline(header_stream, line))
                {
                    if (line.find("Status:") == 0)
                    {
                        std::string status = line.substr(7);
                        size_t space_pos = status.find(' ');
                        if (space_pos != std::string::npos)
                        {
                            response.status_code = std::stoi(trim(status.substr(0, space_pos)));
                            response.status_text = trim(status.substr(space_pos + 1));
                        }
                    }
                    else if (line.find("Content-Type:") == 0)
                    {
                        response.headers["Content-Type"] = trim(line.substr(13));
                    }
                }
                response.body = body;
            }
            else
            {
                response.body = cgi_output;
                if (response.headers.find("Content-Type") == response.headers.end())
                {
                    response.headers["Content-Type"] = "text/html";
                }
            }
        }
        else
        {
            // 通常のファイル
            std::string content = read_file(file_path);
            if (content.empty())
            {
                response.status_code = 500;
                response.status_text = "Internal Server Error";
                response.headers["Content-Type"] = "text/html";
                response.body = "<html><body><h1>500 Internal Server Error</h1></body></html>";
            }
            else
            {
                response.headers["Content-Type"] = get_mime_type(file_path);
                response.body = content;
            }
        }
    }
    else
    {
        response.status_code = 404;
        response.status_text = "Not Found";
        response.headers["Content-Type"] = "text/html";
        response.body = "<html><body><h1>404 Not Found</h1></body></html>";
    }

    response.headers["Content-Length"] = std::to_string(response.body.length());

    // ログ出力
    log_request(request.path, response.status_code, response.status_text);

    return response;
}

// クライアント処理
void handle_client(SOCKET client_socket, const Config &config, const std::string &client_ip)
{
    if (!server_running)
    {
        close(client_socket);
        return;
    }

    char buffer[4096];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received > 0 && server_running)
    {
        buffer[bytes_received] = '\0';

        HttpRequest request = parse_http_request(std::string(buffer));
        HttpResponse response = handle_request(request, config, client_ip);
        std::string response_str = create_http_response(response);

        send(client_socket, response_str.c_str(), response_str.length(), 0);
    }

    close(client_socket);
}

// サーバー開始
void start_server(const Config &config)
{
    // シグナルハンドラーの設定
    signal(SIGINT, signal_handler);

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET)
    {
        std::cerr << "Failed to create socket" << std::endl;
        return;
    }

    global_server_socket = server_socket;

    // SO_REUSEADDR オプション
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config.port);
    server_addr.sin_addr.s_addr = inet_addr(config.host.c_str());

    if (bind(server_socket, (sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
    {
        std::cerr << "Failed to bind socket" << std::endl;
        close(server_socket);
        return;
    }

    if (listen(server_socket, 10) == SOCKET_ERROR)
    {
        std::cerr << "Failed to listen on socket" << std::endl;
        close(server_socket);
        return;
    }

    std::cout << "Server starting on " << "http://" << config.host << ":" << config.port << std::endl;
    std::cout << "Document root: " << config.root_dir << std::endl;
    std::cout << "Press Ctrl+C to stop the server" << std::endl;
    std::cout << "======================================" << std::endl;

    while (server_running)
    {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // タイムアウト付きでaccept
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int select_result = select(server_socket + 1, &read_fds, NULL, NULL, &timeout);

        if (select_result < 0)
        {
            if (errno == EINTR)
            {
                // シグナルによる割り込み
                continue;
            }
            if (server_running)
            {
                std::cerr << "Select failed" << std::endl;
            }
            break;
        }
        else if (select_result == 0)
        {
            // タイムアウト
            continue;
        }

        SOCKET client_socket = accept(server_socket, (sockaddr *)&client_addr, &client_len);

        if (client_socket == INVALID_SOCKET)
        {
            if (server_running)
            {
                std::cerr << "Accept failed" << std::endl;
            }
            continue;
        }

        if (!server_running)
        {
            close(client_socket);
            break;
        }

        // クライアントのIPアドレスを取得
        std::string client_ip = inet_ntoa(client_addr.sin_addr);

        // 新しいスレッドでクライアントを処理
        std::thread client_thread(handle_client, client_socket, config, client_ip);
        client_thread.detach();
    }

    close(server_socket);
    global_server_socket = INVALID_SOCKET;
    std::cout << "サーバーが停止しました。" << std::endl;
}

// ヘルプ表示
void show_help()
{
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
void show_version()
{
    std::cout << "Simple Web Server v1.0" << std::endl;
    std::cout << "Built with C++11" << std::endl;
}

// メイン関数
int main(int argc, char *argv[])
{
    Config config;

    // コマンドライン引数の解析
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--help")
        {
            show_help();
            return 0;
        }
        else if (arg == "-v" || arg == "--version")
        {
            show_version();
            return 0;
        }
        else if ((arg == "-r" || arg == "--root_dir") && i + 1 < argc)
        {
            config.root_dir = argv[++i];
        }
        else if ((arg == "-p" || arg == "--port") && i + 1 < argc)
        {
            config.port = std::atoi(argv[++i]);
        }
        else if ((arg == "-h" || arg == "--host") && i + 1 < argc)
        {
            config.host = argv[++i];
        }
    }

    // ルートディレクトリの存在確認
    if (!dir_exists(config.root_dir))
    {
        std::cerr << "Error: Root directory '" << config.root_dir << "' does not exist" << std::endl;
        return 1;
    }

    start_server(config);
    return 0;
}