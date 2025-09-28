#!/usr/bin/perl
#
# tth - tiny tiny httpd v1.0 (Perl版)
# Linux用の軽量HTTPサーバー実装
# 静的ファイル配信とCGI実行をサポート
# マルチスレッド対応でHTTP/1.1準拠
#

use strict;
use warnings;
use IO::Socket::INET;
use File::Spec;
use File::Basename;
use URI::Escape;
use POSIX qw(strftime WNOHANG);
use Getopt::Long;
use Cwd qw(abs_path);

# 定数定義
use constant {
    BUFFER_SIZE    => 4096,
    MAX_HEADERS    => 50,
    MAX_PATH_LEN   => 2048,
    MAX_HEADER_LEN => 1024,
};

# グローバル変数
my $server_running = 1;
my $server_socket;

# 設定情報
my %config = (
    root_dir => '.',
    port     => 8080,
    host     => '127.0.0.1'
);

# MIMEタイプマッピング
my %mime_types = (
    '.html' => 'text/html',
    '.htm'  => 'text/html',
    '.css'  => 'text/css',
    '.js'   => 'application/javascript',
    '.json' => 'application/json',
    '.png'  => 'image/png',
    '.jpg'  => 'image/jpeg',
    '.jpeg' => 'image/jpeg',
    '.gif'  => 'image/gif',
    '.txt'  => 'text/plain',
    '.xml'  => 'application/xml',
);

# シグナルハンドラー
$SIG{INT} = sub {
    print "\n\nサーバーを停止しています...\n";
    $server_running = 0;
    $server_socket->close() if $server_socket;
    print "サーバーが正常に停止しました。\n";
    exit(0);
};

# 子プロセスの終了を処理してゾンビプロセスを防ぐ
$SIG{CHLD} = sub {
    while ( ( my $pid = waitpid( -1, WNOHANG ) ) > 0 ) {}
};

# 文字列の前後の空白を除去
sub trim {
    my $str = shift;
    $str =~ s/^\s+|\s+$//g;
    return $str;
}

# ファイルがCGIスクリプトかどうかを判定
sub is_cgi_file {
    my $file_path = shift;

    # 拡張子による判定
    my ( $name, $path, $ext ) = fileparse( $file_path, qr/\.[^.]*/ );
    if (
        $ext
        && (   $ext eq '.cgi'
            || $ext eq '.pl'
            || $ext eq '.py'
            || $ext eq '.sh'
            || $ext eq '.rb' )
      )
    {
        return 1;
    }

    # shebangによる判定
    if ( open my $fh, '<', $file_path ) {
        my $first_line = <$fh>;
        close $fh;
        return 1 if $first_line && $first_line =~ /^#!/;
    }

    return 0;
}


# URLデコード
sub url_decode {
    my $str = shift;
    $str =~ s/\+/ /g;
    $str =~ s/%([0-9A-Fa-f]{2})/chr(hex($1))/eg;
    return $str;
}

# ファイル拡張子からMIMEタイプを取得
sub get_mime_type {
    my $path = shift;
    my ( $name, $dir, $ext ) = fileparse( $path, qr/\.[^.]*/ );
    return $mime_types{ lc($ext) } || 'application/octet-stream';
}

# ファイルを読み込んでメモリにロード
sub read_file {
    my $path = shift;

    open my $fh, '<:raw', $path or return undef;
    local $/;
    my $content = <$fh>;
    close $fh;

    return $content;
}

# URLパスからパスとクエリストリングを分離
sub parse_path_and_query {
    my $path = shift;

    if ( $path =~ /^([^?]*)\?(.*)$/ ) {
        return ( $1, $2 );
    }
    return ( $path, '' );
}

# CGIスクリプト名とPATH_INFOを分離
sub parse_script_and_path_info {
    my ( $path, $root_dir ) = @_;

    my @parts        = split '/', $path;
    my $current_path = '';

    for my $i ( 0 .. $#parts ) {
        next if $parts[$i] eq '';
        $current_path .= '/' . $parts[$i];

        my $file_path = File::Spec->catfile( $root_dir, $current_path );
        if ( -f $file_path ) {
            my $script_name = $current_path;
            my $path_info   = '';

            if ( $i < $#parts ) {
                $path_info = '/' . join( '/', @parts[ $i + 1 .. $#parts ] );
            }

            return ( $script_name, $path_info );
        }
    }

    return ( $path, '' );
}

# スクリプトのshebangからインタープリターを取得
sub get_interpreter_from_shebang {
    my $script_path = shift;

    open my $fh, '<', $script_path or return undef;
    my $first_line = <$fh>;
    close $fh;

    return undef unless $first_line && $first_line =~ /^#!/;

    $first_line = trim($first_line);
    $first_line =~ s/^#!\s*//;
    $first_line =~ s/\s.*$//;    # 引数を除去

    return $first_line;
}

# CGIスクリプトを実行して出力を取得
sub execute_cgi {
    my ( $script_path, $request, $root_dir, $client_ip ) = @_;

    my ( $clean_path, $query_string ) =
      parse_path_and_query( $request->{path} );
    my ( $script_name, $path_info ) =
      parse_script_and_path_info( $clean_path, $root_dir );

    my $interpreter = get_interpreter_from_shebang($script_path);

    # CGI環境変数を設定
    local %ENV = %ENV;
    $ENV{REQUEST_METHOD}    = $request->{method};
    $ENV{SCRIPT_NAME}       = $script_name;
    $ENV{PATH_INFO}         = $path_info;
    $ENV{PATH_TRANSLATED}   = File::Spec->catfile( $root_dir, $path_info );
    $ENV{QUERY_STRING}      = $query_string;
    $ENV{CONTENT_LENGTH}    = length( $request->{body} || '' );
    $ENV{GATEWAY_INTERFACE} = 'CGI/1.1';
    $ENV{SERVER_NAME}       = $config{host};
    $ENV{SERVER_PORT}       = $config{port};
    $ENV{SERVER_PROTOCOL}   = $request->{version};
    $ENV{SERVER_SOFTWARE}   = 'tth/1.0';
    $ENV{REMOTE_ADDR}       = $client_ip;
    $ENV{REMOTE_HOST}       = $client_ip;

    # ヘッダーから環境変数を設定
    for my $header ( @{ $request->{headers} } ) {
        if ( $header =~ /^Content-Type:\s*(.+)$/i ) {
            $ENV{CONTENT_TYPE} = trim($1);
        }
    }

    # スクリプトを実行
    my $command;
    if ( $interpreter && -x $interpreter ) {
        $command = "$interpreter $script_path";
    }
    else {
        $command = $script_path;
    }

    # 作業ディレクトリを変更
    my $old_cwd = Cwd::getcwd();
    chdir $root_dir;

    my $output = '';
    if ( open my $pipe, '-|', $command ) {
        local $/;
        $output = <$pipe>;
        close $pipe;
    }
    else {
        $output =
          "Status: 500 Internal Server Error\r\n\r\nCGI execution failed";
    }

    # 元のディレクトリに戻る
    chdir $old_cwd;

    return $output;
}

# リクエストのログを色付きで出力
sub log_request {
    my ( $path, $status_code, $status_text ) = @_;

    my $timestamp = strftime( "%Y-%m-%d %H:%M:%S", localtime );

    my $color = '';
    if ( $status_code >= 200 && $status_code < 300 ) {
        $color = "\033[32m";
    }
    elsif ( $status_code >= 300 && $status_code < 400 ) {
        $color = "\033[33m";
    }
    elsif ( $status_code >= 400 && $status_code < 500 ) {
        $color = "\033[31m";
    }
    elsif ( $status_code >= 500 ) {
        $color = "\033[35m";
    }

    printf "\033[36m%s\033[0m %s%d\033[0m \033[34m%s\033[0m %s%s\033[0m\n",
      $timestamp, $color, $status_code, $path, $color, $status_text;
}

# 生のHTTPリクエスト文字列をパース
sub parse_http_request {
    my $raw_request = shift;

    my %request = (
        method  => '',
        path    => '',
        version => '',
        headers => [],
        body    => ''
    );

    my @lines = split /\r?\n/, $raw_request;

    # リクエストラインをパース
    if ( @lines && $lines[0] =~ /^(\S+)\s+(\S+)\s+(\S+)/ ) {
        $request{method}  = $1;
        $request{path}    = $2;
        $request{version} = $3;
    }

    # ヘッダーをパース
    my $i = 1;
    while ( $i < @lines && $lines[$i] ne '' ) {
        push @{ $request{headers} }, $lines[$i];
        $i++;
    }

    # ボディをパース
    if ( $i < @lines ) {
        $request{body} = join( "\n", @lines[ $i + 1 .. $#lines ] );
    }

    return \%request;
}

# HTTPレスポンスを作成
sub create_http_response {
    my $res = shift;

    my $res_str = sprintf "HTTP/1.1 %d %s\r\n",
      $res->{status_code}, $res->{status_text};

    for my $header ( @{ $res->{headers} } ) {
        $res_str .= "$header\r\n";
    }

    $res_str .= "\r\n";

    if ( $res->{body} ) {
        $res_str .= $res->{body};
    }

    return $res_str;
}

# HTTPリクエストを処理してレスポンスを生成
sub handle_request {
    my ( $req, $client_ip ) = @_;

    my %res = (
        status_code => 200,
        status_text => 'OK',
        headers     => ['Server: tth/1.0'],
        body        => ''
    );

    my $decoded_path = url_decode( $req->{path} );

    # パストラバーサル攻撃を防ぐ
    if ( $decoded_path =~ /\.\./ ) {
        $res{status_code} = 403;
        $res{status_text} = 'Forbidden';
        $res{headers} = [ 'Server: tth/1.0', 'Content-Type: text/html' ];
        $res{body}    = '<html><body><h1>403 Forbidden</h1></body></html>';
        push @{ $res{headers} },
          'Content-Length: ' . length( $res{body} );

        log_req( $req->{path}, $res{status_code},
            $res{status_text} );
        return \%res;
    }

    # ファイルパスを構築
    my $file_path;
    if ( $decoded_path eq '/' || $decoded_path eq '' ) {
        $file_path = File::Spec->catfile( $config{root_dir}, 'index.html' );
    }
    else {
        $file_path = File::Spec->catfile( $config{root_dir}, $decoded_path );
    }

    if ( -f $file_path ) {
        if ( is_cgi_file($file_path) ) {

            # CGIスクリプトを実行
            my $cgi_output =
              execute_cgi( $file_path, $req, $config{root_dir},
                $client_ip );

            # CGI出力をヘッダーとボディに分離
            if ( $cgi_output =~ /^(.*?)\r?\n\r?\n(.*)$/s ) {
                my ( $headers_part, $body_part ) = ( $1, $2 );

                # CGIヘッダーを解析
                for my $line ( split /\r?\n/, $headers_part ) {
                    if ( $line =~ /^Status:\s*(\d+)\s*(.*)$/i ) {
                        $res{status_code} = $1;
                        $res{status_text} = $2 || 'Unknown';
                    }
                    elsif ( $line =~ /^Content-Type:\s*(.+)$/i ) {
                        push @{ $res{headers} },
                          "Content-Type: " . trim($1);
                    }
                }

                $res{body} = $body_part;
            }
            else {
                $res{body} = $cgi_output;
                push @{ $res{headers} }, 'Content-Type: text/html';
            }
        }
        else {
            # 静的ファイルを配信
            my $content = read_file($file_path);
            if ( !defined $content ) {
                $res{status_code} = 500;
                $res{status_text} = 'Internal Server Error';
                push @{ $res{headers} }, 'Content-Type: text/html';
                $res{body} =
'<html><body><h1>500 Internal Server Error</h1></body></html>';
            }
            else {
                my $mime_type = get_mime_type($file_path);
                push @{ $res{headers} }, "Content-Type: $mime_type";
                $res{body} = $content;
            }
        }
    }
    else {
        # ファイルが見つからない
        $res{status_code} = 404;
        $res{status_text} = 'Not Found';
        push @{ $res{headers} }, 'Content-Type: text/html';
        $res{body} = '<html><body><h1>404 Not Found</h1></body></html>';
    }

    # Content-Lengthヘッダーを追加
    push @{ $res{headers} },
      'Content-Length: ' . length( $res{body} );

    log_request( $req->{path}, $res{status_code},
        $res{status_text} );
    return \%res;
}

# クライアント接続をスレッドで処理
sub handle_client {
    my $client_socket = shift;

    return unless $server_running;

    my $client_addr = $client_socket->peerhost();

    my $buffer = '';
    $client_socket->recv( $buffer, BUFFER_SIZE );

    if ( $buffer && $server_running ) {
        my $request      = parse_http_request($buffer);
        my $response     = handle_request( $request, $client_addr );
        my $response_str = create_http_response($response);

        $client_socket->send($response_str);
    }

    $client_socket->close();
}

# HTTPサーバーを開始してメインループを実行
sub start_server {
    $server_socket = IO::Socket::INET->new(
        LocalHost => $config{host},
        LocalPort => $config{port},
        Proto     => 'tcp',
        Listen    => 1,
        Reuse     => 1
    ) or die "サーバーソケットの作成に失敗しました: $!\n";

    printf "Server starting on http://%s:%d\n", $config{host}, $config{port};
    printf "Document root: %s\n", $config{root_dir};
    print "Press Ctrl+C to stop the server\n";
    print "======================================\n";

    while ($server_running) {
        my $client_socket = $server_socket->accept();
        next unless $client_socket && $server_running;

        # 新しいプロセスでクライアントを処理
        my $pid = fork();
        if ( !defined $pid ) {
            warn "fork failed: $!";
            $client_socket->close();
            next;
        }

        if ( $pid == 0 ) {

            # 子プロセス
            handle_client($client_socket);
            exit(0);
        }
        else {
            # 親プロセス
            $client_socket->close();
        }
    }

    $server_socket->close();
    print "サーバーが停止しました。\n";
}

# ヘルプメッセージを表示
sub show_help {
    print "tth - tiny tiny httpd v1.0\n";
    print "Usage: perl tth.pl [options]\n";
    print "\n";
    print "Options:\n";
    print
"  -r, --root-dir DIR    Set document root directory (default: current directory)\n";
    print "  -p, --port PORT       Set port number (default: 8080)\n";
    print "  -h, --host HOST       Set host address (default: 127.0.0.1)\n";
    print "  -v, --version         Show version information\n";
    print "  --help               Show this help message\n";
}

# バージョン情報を表示
sub show_version {
    print "tth - tiny tiny httpd v1.0\n";
    print "Built with Perl\n";
}

# メイン処理 - コマンドライン引数を処理してサーバーを起動
sub main {
    my $help    = 0;
    my $version = 0;

    GetOptions(
        'root-dir|r=s' => \$config{root_dir},
        'port|p=i'     => \$config{port},
        'host|h=s'     => \$config{host},
        'help'         => \$help,
        'version|v'    => \$version,
    ) or die "コマンドライン引数の解析に失敗しました\n";

    if ($help) {
        show_help();
        return 0;
    }

    if ($version) {
        show_version();
        return 0;
    }

    # ドキュメントルートの存在確認
    unless ( -d $config{root_dir} ) {
        die "Error: Root directory '$config{root_dir}' does not exist\n";
    }

    # 絶対パスに変換
    $config{root_dir} = abs_path( $config{root_dir} );

    start_server();
    return 0;
}

# メイン実行
exit main() if __FILE__ eq $0;
