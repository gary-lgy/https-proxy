CC = /usr/bin/gcc
SHELL = /usr/bin/bash
CFLAGS = -B/usr/bin/ -Wall -Wextra --std=gnu11 -D_GNU_SOURCE
LFLAGS = -lpthread
SRC_FILES = proxy.c http.c log.c util.c tunnel_conn.c \
            states/accepted.c states/connecting.c states/tunneling.c \
            lib/asyncaddrinfo/asyncaddrinfo.c
OUT_DIR = out
BIN = proxy

.PHONY: all debug_build candidate_build build clean

all: debug_build

debug_build: clean
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -g -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

candidate_build: clean
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -DNO_DEBUG_LOG -O2 -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

build: clean
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -DNO_LOG -O2 -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

clean:
	rm -rf $(OUT_DIR)