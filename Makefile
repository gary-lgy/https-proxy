CC = /usr/bin/gcc
CFLAGS = -Wall -Wextra --std=gnu11 -D_GNU_SOURCE
SRC_FILES = proxy.c error.c http_connect.c log.c
OUT_DIR = out
BIN = proxy

.PHONY: all debug_build build clean

all: debug_build

debug_build: clean
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -g -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

build: clean
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -DNDEBUG -o $(OUT_DIR)/$(BIN) $(SRC_FILES) $(LFLAGS)

clean:
	rm -rf $(OUT_DIR)