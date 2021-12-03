CC = /usr/bin/gcc
SHELL = /usr/bin/bash
CFLAGS = -B/usr/bin/ -Wall -Wextra --std=gnu11 -D_GNU_SOURCE
LFLAGS = -lpthread
SRC_FILES = proxy.c http.c log.c util.c tunnel_conn.c \
            states/accepted.c states/connecting.c states/tunneling.c \
            lib/asyncaddrinfo/asyncaddrinfo.c \
            poll.c proxy_server.c
OUT_DIR = out
BIN = proxy

.PHONY: all debug dev prod clean

all: prod

# Verbose logging, debug symbols
debug: clean
	$(CC) $(CFLAGS) -g -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

# Less verbose logging, -O2, no debug symbols
dev: clean
	$(CC) $(CFLAGS) -DNO_DEBUG_LOG -O2 -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

# No logging, -O2, no debug symbols
prod: clean
	$(CC) $(CFLAGS) -DNO_LOG -O2 -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

clean:
	rm -rf $(OUT_DIR)
	mkdir -p $(OUT_DIR)
