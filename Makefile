# tth - tiny tiny httpd
# C言語で書かれたシンプルなHTTPサーバー
# 静的ファイルとCGIスクリプトをサポート
#
# 使い方:
#   make        - ビルド
#   make clean  - 削除
#   make install- インストール

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lpthread
TARGET = tth
SOURCE = tth.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all clean install