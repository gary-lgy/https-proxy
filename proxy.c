#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "lib/asyncaddrinfo/asyncaddrinfo.h"
#include "log.h"
#include "states/epoll_cb.h"
#include "util.h"

#define CONNECT_BACKLOG 512
#define EPOLL_MAX_EVENTS 64
#define DEFAULT_THREAD_COUNT 8
#define MAX_BLACKLIST_LEN 100

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

struct event_loop_args {
  unsigned short thread_id;
  bool telemetry_enabled;
  int listening_socket;
  char** blacklist;
  int blacklist_len;
};

void handle_connections_in_event_loop(struct event_loop_args* args) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    die(hsprintf("failed to create epoll instance: %s", errno2s(errno)));
  }

  struct epoll_event event, events[EPOLL_MAX_EVENTS];

  // Configure `event.data.ptr` to be NULL when there are events on listening socket
  event.data.ptr = NULL;
  // Since we will call `accept4` until there are no more incoming connections,
  // and edge-triggered is more efficient than level-triggered,
  // we can register edge-triggered notification for read events on the listening socket
  event.events = EPOLLIN | EPOLLET;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, args->listening_socket, &event) < 0) {
    die(hsprintf("failed to add listening socket %d into epoll: %s", args->listening_socket, errno2s(errno)));
  }

  // event loop
  while (1) {
    int num_events = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, -1);
    DEBUG_LOG("epoll_wait returned %d", num_events);
    if (num_events < 0) {
      die(hsprintf("epoll_wait error: %s", errno2s(errno)));
    }

    for (int i = 0; i < num_events; i++) {
      if (events[i].data.ptr == NULL) {
        // events on listening socket
        if (events[i].events & EPOLLERR) {
          DEBUG_LOG("epoll reported error on listening socket");
        }
        if (!(events[i].events & EPOLLIN)) {
          DEBUG_LOG("listening socket is not readable but epoll woke us up anyway");
          continue;
        }
        accept_incoming_connections(
            epoll_fd, args->listening_socket, args->telemetry_enabled, args->blacklist, args->blacklist_len);
      } else {
        // events on existing connection
        struct epoll_cb* cb = events[i].data.ptr;
        if (cb->type == cb_type_accepted) {
          handle_accepted_cb(epoll_fd, (struct epoll_accepted_cb*)cb, events[i].events);
        } else if (cb->type == cb_type_connecting) {
          handle_connecting_cb(epoll_fd, (struct epoll_connecting_cb*)cb, events[i].events);
        } else if (cb->type == cb_type_tunneling) {
          handle_tunneling_cb(epoll_fd, (struct epoll_tunneling_cb*)cb, events[i].events);
        }
      }
    }
  }

  if (close(epoll_fd) < 0) {
    die(hsprintf("failed to close epoll instance: %s", errno2s(errno)));
  }
}

void* handle_connections_in_event_loop_pthread_wrapper(void* raw_args) {
  struct event_loop_args* args = raw_args;
  thread_id__ = args->thread_id;  // for logging purpose
  handle_connections_in_event_loop(args);
  return NULL;
}

int read_blacklist(const char* blacklist_path, char*** blacklist_ptr) {
  char** blacklist = *blacklist_ptr = calloc(MAX_BLACKLIST_LEN, sizeof(char*));

  FILE* fp = fopen(blacklist_path, "r");
  if (fp == NULL) {
    die(hsprintf("could not open file: '%s'", blacklist_path));
  }

  size_t buffer_len = 0;
  int blacklist_len = 0;

  while (1) {
    if (blacklist_len >= MAX_BLACKLIST_LEN) {
      die("too many entries in the blacklist. Only up to 100 is supported.");
    }

    if (getline(&blacklist[blacklist_len], &buffer_len, fp) == -1) {
      free(blacklist[blacklist_len]);
      break;
    }

    size_t char_count = strcspn(blacklist[blacklist_len], "\r\n");
    if (char_count == 0) {
      // empty line
      continue;
    }
    blacklist[blacklist_len][char_count] = '\0';
    DEBUG_LOG("Read blacklist entry %d: %s", blacklist_len, blacklist[blacklist_len]);
    blacklist_len++;
  }

  fclose(fp);

  return blacklist_len;
}

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    die(hsprintf("Usage: %s <port> <flag_telemetry> <path to blacklist file> [thread count]", argv[0]));
  }

  char* endptr;

  unsigned short listening_port = strtol(argv[1], &endptr, 10);
  if (*endptr != '\0') {
    // the raw string contains unrecognized characters
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

  const char* blacklist_path = argv[3];
  char** blacklist;
  int blacklist_len = read_blacklist(blacklist_path, &blacklist);

  unsigned short thread_count = DEFAULT_THREAD_COUNT;
  if (argc == 5) {
    thread_count = strtol(argv[4], &endptr, 10);
    if (*endptr != '\0') {
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
  printf("- path to blacklist file:                  %s\n", blacklist_path);
  printf("- number of entries in the blacklist file: %d\n", blacklist_len);
  printf("- number of connection threads:            %hu\n", connection_threads);
  printf("- number of async addrinfo (DNS) threads:  %hu\n", asyncaddrinfo_threads);

  int listening_socket = create_bind_listen(listening_port);

  // start the addr info lookup threads
  asyncaddrinfo_init(asyncaddrinfo_threads);

  // start the connection threads
  struct event_loop_args args_list[connection_threads];
  for (int i = 0; i < connection_threads; i++) {
    args_list[i].listening_socket = listening_socket;
    args_list[i].telemetry_enabled = telemetry_enabled;
    args_list[i].thread_id = i;
    args_list[i].blacklist = blacklist;
    args_list[i].blacklist_len = blacklist_len;
  }

  pthread_t workers[connection_threads - 1];
  for (int i = 0; i < connection_threads - 1; i++) {
    // child threads will have id from 1 onwards
    // the main thread will be thread 0
    if (0 != pthread_create(&workers[i], NULL, handle_connections_in_event_loop_pthread_wrapper, &args_list[i + 1])) {
      die(hsprintf("error creating thread %d: %s", i + 1, errno2s(errno)));
    }
  }

  printf("Accepting requests\n");
  // run another event loop on the main thread
  handle_connections_in_event_loop_pthread_wrapper(&args_list[0]);

  // We will never reach here, the cleanup code below is just for completeness' sake

  if (close(listening_socket) < 0) {
    die(hsprintf("failed to close listening socket: %s", errno2s(errno)));
  }

  for (int i = 0; i < connection_threads; i++) {
    if (0 != pthread_join(workers[i], NULL)) {
      die(hsprintf("error joining thread %d: %s", i + 1, errno2s(errno)));
    }
  }

  for (int i = 0; i < blacklist_len; i++) {
    free(blacklist[i]);
  }
  free(blacklist);

  asyncaddrinfo_cleanup();

  return 0;
}