# Makefile for Simple Web Server

CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2
TARGET = tth
SOURCE = tth.cpp

# OS detection
UNAME_S := $(shell uname -s)
ifeq ($(OS),Windows_NT)
    # Windows (MinGW)
    CXXFLAGS += -D_WIN32_WINNT=0x0601
    LDFLAGS = -lws2_32
    TARGET := $(TARGET).exe
    RM = del /Q
    CP = copy
else ifeq ($(UNAME_S),Linux)
    # Linux
    LDFLAGS = -lpthread
    RM = rm -f
    CP = cp
else ifeq ($(UNAME_S),Darwin)
    # macOS
    LDFLAGS = -lpthread
    RM = rm -f
    CP = cp
else
    # Default Unix-like
    LDFLAGS = -lpthread
    RM = rm -f
    CP = cp
endif

.PHONY: all clean install debug release test

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

debug: CXXFLAGS += -g -DDEBUG
debug: $(TARGET)

release: CXXFLAGS += -DNDEBUG
release: $(TARGET)

clean:
	$(RM) $(TARGET)

install: $(TARGET)
ifeq ($(OS),Windows_NT)
	$(CP) $(TARGET) C:\Windows\System32\
else
	sudo $(CP) $(TARGET) /usr/local/bin/
endif

test: $(TARGET)
	@echo "Creating test files..."
ifeq ($(OS),Windows_NT)
	@echo ^<!DOCTYPE html^>^<html^>^<head^>^<title^>Test^</title^>^</head^>^<body^>^<h1^>Test Page^</h1^>^</body^>^</html^> > test.html
	@echo @echo off > test.bat
	@echo echo Content-Type: text/html >> test.bat
	@echo echo. >> test.bat
	@echo echo ^<h1^>Windows CGI Test^</h1^> >> test.bat
	@echo echo ^<p^>Time: %date% %time%^</p^> >> test.bat
else
	@echo '<!DOCTYPE html><html><head><title>Test</title></head><body><h1>Test Page</h1></body></html>' > test.html
	@echo '#!/bin/bash' > test.cgi
	@echo 'echo "Content-Type: text/html"' >> test.cgi
	@echo 'echo ""' >> test.cgi
	@echo 'echo "<h1>CGI Test</h1>"' >> test.cgi
	@echo 'echo "<p>Time: $(date)</p>"' >> test.cgi
	@chmod +x test.cgi
endif
	@echo "Test files created. Start server with: ./$(TARGET)"
	@echo "Then visit: http://localhost:8080/test.html"
ifeq ($(OS),Windows_NT)
	@echo "           : http://localhost:8080/test.bat"
else
	@echo "           : http://localhost:8080/test.cgi"
endif

help:
	@echo "Simple Web Server Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build the web server (default)"
	@echo "  debug    - Build with debug symbols"
	@echo "  release  - Build optimized release version"
	@echo "  clean    - Remove built files"
	@echo "  install  - Install to system directory"
	@echo "  test     - Create test files"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Usage:"
	@echo "  make"
	@echo "  make clean"
	@echo "  make install"