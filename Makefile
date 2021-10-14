CC = /usr/bin/gcc
SHELL = /usr/bin/bash
CFLAGS = -Wall -Wextra --std=gnu11 -D_GNU_SOURCE
LFLAGS = -lpthread
SRC_FILES = proxy.c http.c log.c util.c tunnel_conn.c epoll_cb.c
OUT_DIR = out
BIN = proxy

.PHONY: all debug_build build clean

all: debug_build

debug_build: clean
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -g -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

build: clean
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -DNDEBUG -O2 -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

clean:
	rm -rf $(OUT_DIR)