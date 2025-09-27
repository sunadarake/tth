#!/usr/bin/perl

use strict;
use warnings;
use utf8;
binmode STDOUT, ':utf8';

print "Content-Type: text/html; charset=UTF-8\r\n\r\n";
print "<!DOCTYPE html><html><head>";
print "<meta charset='UTF-8'>";
print "<title>Perl CGI</title></head><body>\n";
print "<h1>🐪 Perl CGI テスト</h1>\n";
print "<p><strong>現在時刻:</strong> " . localtime() . "</p>\n";
print "<p><strong>サーバー:</strong> $ENV{'SERVER_SOFTWARE'}</p>\n" if $ENV{'SERVER_SOFTWARE'};
print "<p><strong>リクエストメソッド:</strong> $ENV{'REQUEST_METHOD'}</p>\n" if $ENV{'REQUEST_METHOD'};
print "<p><strong>スクリプト名:</strong> $ENV{'SCRIPT_NAME'}</p>\n" if $ENV{'SCRIPT_NAME'};
print "<p><strong>日本語テスト:</strong> こんにちは！ 🌸 UTF-8 エンコーディングが正常に動作しています。</p>\n";
print "<p><a href='/'>← ホームに戻る</a></p>\n";
print "</body></html>\n";