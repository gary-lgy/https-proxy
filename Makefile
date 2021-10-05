CC = /usr/bin/gcc
CFLAGS = --std=c11 -Wall
OUT_DIR = out

all: proxy

proxy:
	mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -o $(OUT_DIR)/proxy proxy.c $(LFLAGS)

clean:
	rm -rf $(OUT_DIR)