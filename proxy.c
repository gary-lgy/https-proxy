#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "log.h"
#include "states/epoll_cb.h"
#include "util.h"

#define CONNECT_BACKLOG 512
#define EPOLL_MAX_EVENTS 64
#define DEFAULT_MAX_THREADS 8

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
};

void event_loop_main(struct event_loop_args* args) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    die(hsprintf("failed to create epoll instance: %s", errno2s(errno)));
  }

  struct epoll_event event, events[EPOLL_MAX_EVENTS];

  // NULL means we have events on listening socket
  event.data.ptr = NULL;
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
      break;
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
        accept_incoming_connections(epoll_fd, args->listening_socket, args->telemetry_enabled);
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

void* event_loop_main_pthread_wrapper(void* raw_args) {
  struct event_loop_args* args = raw_args;
  thread_id__ = args->thread_id;
  event_loop_main(args);
  free(args);
  return NULL;
}

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    die(hsprintf("Usage: %s <port> <flag_telemetry> <filename of blacklist> [max threads]", argv[0]));
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
    die(hsprintf("expected flag_telemetry to be either 0 or 1, got %s", argv[2]));
  }

  // TODO: blacklist
  const char* blacklist_filename = argv[3];

  unsigned short max_threads = DEFAULT_MAX_THREADS;
  if (argc == 5) {
    max_threads = strtol(argv[4], &endptr, 10);
    if (*endptr != '\0') {
      die(hsprintf("failed to parse max threads '%s'", argv[4]));
    }
    if (max_threads < 2) {
      die("at least 2 threads are required");
    }
  }

  printf("- listening port:        %hu\n", listening_port);
  printf("- telemetry enabled:     %s\n", telemetry_enabled ? "yes" : "no");
  printf("- blacklist filename:    %s\n", blacklist_filename);
  printf("- max number of threads: %hu\n", max_threads);

  int listening_socket = create_bind_listen(listening_port);

  pthread_t workers[max_threads - 1];
  for (int i = 0; i < max_threads - 1; i++) {
    struct event_loop_args* args = malloc(sizeof(struct event_loop_args));
    args->listening_socket = listening_socket;
    args->telemetry_enabled = telemetry_enabled;
    args->thread_id = i + 1;
    pthread_create(&workers[i], NULL, event_loop_main_pthread_wrapper, args);
  }

  printf("Accepting requests\n");
  // main thread does the same work
  struct event_loop_args args = {
      .listening_socket = listening_socket,
      .telemetry_enabled = telemetry_enabled,
      .thread_id = 0,
  };
  event_loop_main_pthread_wrapper(&args);

  if (close(listening_socket) < 0) {
    die(hsprintf("failed to close listening socket: %s", errno2s(errno)));
  }

  return 0;
}