#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "lib/asyncaddrinfo/asyncaddrinfo.h"
#include "log.h"
#include "poll.h"
#include "proxy/proxy_server.h"
#include "util.h"

#define CONNECT_BACKLOG 512
#define DEFAULT_THREAD_COUNT 8
#define MAX_BLOCKLIST_LEN 100

int create_bind_listen(unsigned short port) {
  int listening_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (listening_socket < 0) {
    die(hsprintf("failed to create listening socket: %s", errno2s(errno)));
  }

  struct sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(port);

  if (bind(listening_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
    die(hsprintf("failed to bind listening socket to port: %s", errno2s(errno)));
  }

  if (listen(listening_socket, CONNECT_BACKLOG) < 0) {
    die(hsprintf("failed to listen: %s", errno2s(errno)));
  }

  return listening_socket;
}

struct connection_thread_args {
  unsigned short thread_id;
  struct proxy_server* server;
};

void handle_connections(struct proxy_server* server) {
  struct poll* p = poll_create();
  if (p == NULL) {
    die(hsprintf("failed to create poll instance: %s", errno2s(errno)));
  }

  // Since we will call `accept4` until there are no more incoming connections,
  // we can register edge-triggered notification for read events on the listening socket.
  // Edge-triggered is more efficient than level-triggered.
  if (poll_wait_for_readability(
          p, server->listening_socket, server, false, true, (poll_callback)accept_incoming_connections) < 0) {
    die(hsprintf("failed to register readability notification for listening socket: %s", errno2s(errno)));
  }

  // start the event loop and run until termination
  if (poll_run(p) < 0) {
    die(hsprintf("poll_run returned error: %s", errno2s(errno)));
  }

  poll_destroy(p);
}

void* handle_connections_pthread_wrapper(void* raw_args) {
  struct connection_thread_args* args = raw_args;
  thread_id__ = args->thread_id;  // to identify the current thread in logging
  handle_connections(args->server);
  return NULL;
}

int read_blocklist(const char* blocklist_path, char*** blocklist_ptr) {
  char** blocklist = *blocklist_ptr = calloc(MAX_BLOCKLIST_LEN, sizeof(char*));

  FILE* fp = fopen(blocklist_path, "r");
  if (fp == NULL) {
    die(hsprintf("could not open file: '%s'", blocklist_path));
  }

  size_t buffer_len = 0;
  int blocklist_len = 0;

  while (1) {
    if (blocklist_len >= MAX_BLOCKLIST_LEN) {
      die("too many entries in the blocklist. Only up to 100 is supported.");
    }

    if (getline(&blocklist[blocklist_len], &buffer_len, fp) == -1) {
      free(blocklist[blocklist_len]);
      break;
    }

    size_t char_count = strcspn(blocklist[blocklist_len], "\r\n");
    if (char_count == 0) {
      // empty line
      continue;
    }
    blocklist[blocklist_len][char_count] = '\0';
    DEBUG_LOG("Read blocklist entry %d: %s", blocklist_len, blocklist[blocklist_len]);
    blocklist_len++;
  }

  fclose(fp);

  return blocklist_len;
}

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    die(hsprintf("Usage: %s port flag_telemetry path_to_blocklist [thread_count]", argv[0]));
  }

  char* endptr;

  unsigned short listening_port = strtol(argv[1], &endptr, 10);
  if (*argv[1] == '\0'    // empty argument
      || *endptr != '\0'  // unrecognized characters
  ) {
    die(hsprintf("failed to parse port number '%s'", argv[1]));
  }

  bool telemetry_enabled;
  if (strcmp(argv[2], "0") == 0) {
    telemetry_enabled = false;
  } else if (strcmp(argv[2], "1") == 0) {
    telemetry_enabled = true;
  } else {
    die(hsprintf("expected flag_telemetry to be either 0 or 1, got '%s'", argv[2]));
  }

  const char* blocklist_path = argv[3];
  char** blocklist;
  int blocklist_len = read_blocklist(blocklist_path, &blocklist);

  unsigned short thread_count = DEFAULT_THREAD_COUNT;
  if (argc == 5) {
    thread_count = strtol(argv[4], &endptr, 10);
    if (*argv[4] == '\0' || *endptr != '\0') {
      die(hsprintf("failed to parse thread count '%s'", argv[4]));
    }
    if (thread_count < 2) {
      die("at least 2 threads are required");
    }
  }

  // use a quarter of the threads (or minimally 1) for async getaddrinfo
  // use the rest (including the main thread) to run event loops and handle connections
  unsigned short asyncaddrinfo_threads = thread_count / 4;
  if (asyncaddrinfo_threads < 1) {
    asyncaddrinfo_threads = 1;
  }
  unsigned short connection_threads = thread_count - asyncaddrinfo_threads;

  printf("- listening port:                          %hu\n", listening_port);
  printf("- telemetry enabled:                       %s\n", telemetry_enabled ? "yes" : "no");
  printf("- path to blocklist file:                  %s\n", blocklist_path);
  printf("- number of entries in the blocklist file: %d\n", blocklist_len);
  printf("- number of connection threads:            %hu\n", connection_threads);
  printf("- number of async addrinfo (DNS) threads:  %hu\n", asyncaddrinfo_threads);

  // start the addr info lookup threads
  asyncaddrinfo_init(asyncaddrinfo_threads);

  // start the connection threads
  int listening_socket = create_bind_listen(listening_port);
  struct proxy_server server = {
      .listening_socket = listening_socket,
      .telemetry_enabled = telemetry_enabled,
      .blocklist = blocklist,
      .blocklist_len = blocklist_len,
  };

  struct connection_thread_args args_list[connection_threads];
  for (int i = 0; i < connection_threads; i++) {
    args_list[i].thread_id = i;
    args_list[i].server = &server;
  }

  pthread_t workers[connection_threads - 1];
  for (int i = 0; i < connection_threads - 1; i++) {
    // child threads will have id from 1 onwards
    // the main thread will be thread 0
    if (0 != pthread_create(&workers[i], NULL, handle_connections_pthread_wrapper, &args_list[i + 1])) {
      die(hsprintf("error creating thread %d: %s", i + 1, errno2s(errno)));
    }
  }

  printf("Accepting requests\n");
  // run another event loop on the main thread
  handle_connections_pthread_wrapper(&args_list[0]);

  // We will never reach here, the cleanup code below is just for completeness' sake

  if (close(listening_socket) < 0) {
    die(hsprintf("failed to close listening socket: %s", errno2s(errno)));
  }

  for (int i = 0; i < connection_threads; i++) {
    if (0 != pthread_join(workers[i], NULL)) {
      die(hsprintf("error joining thread %d: %s", i + 1, errno2s(errno)));
    }
  }

  for (int i = 0; i < blocklist_len; i++) {
    free(blocklist[i]);
  }
  free(blocklist);

  asyncaddrinfo_cleanup();

  return 0;
}
