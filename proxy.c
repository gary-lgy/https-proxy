#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "http.h"
#include "log.h"
#include "tunnel_conn.h"
#include "util.h"

#define CONNECT_BACKLOG 512
#define EPOLL_MAX_EVENTS 64

inline __attribute__((always_inline)) void die(const char* message) {
  fprintf(stderr, "%s\n", message);
  exit(EXIT_FAILURE);
}

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

int tcp_connect(const struct sockaddr_in* remote_addr) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    return -1;
  }

  if (connect(sock, remote_addr, sizeof(struct sockaddr_in)) < 0) {
    close(sock);
    return -1;
  }

  return sock;
}

int lookup_host_addr(const char* hostname, const char* port, struct addrinfo** results) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  int gai_errno = getaddrinfo(hostname, port, &hints, results);
  if (gai_errno != 0) {
    DEBUG_LOG(hsprintf("target_host resolution failed: %s", gai_strerror(gai_errno)));
    return -1;
  }

  return 0;
}

int tcp_connect_to_target(struct tunnel_conn* conn) {
  struct addrinfo *host_addrs, *rp;

  if (lookup_host_addr(conn->target_host, conn->target_port, &host_addrs) < 0) {
    return -1;
  }

  int sock;
  for (rp = host_addrs; rp != NULL; rp = rp->ai_next) {
    if ((sock = tcp_connect((struct sockaddr_in*)rp->ai_addr)) > 0) {
      // succeeded
      memcpy(conn->target_addr, rp->ai_addr, rp->ai_addrlen);
      break;
    }
  }
  freeaddrinfo(host_addrs);

  if (rp == NULL) {  // No address succeeded
    return -1;
  }

  return sock;
}

/** Read the connect request.
 * Upon return, *read_buffer_ptr will be updated to point to the first character in the buffer that is yet to be
 * processed.
 *
 * Returns the number of characters left in the buffer.
 */

ssize_t handle_http_connect_request(struct tunnel_conn* conn, char** read_buffer_ptr) {
  // TODO: convert to async
  char* read_buffer = conn->client_to_target_buffer;
  read_buffer[0] = '\0';
  ssize_t n_bytes_read = 0;

  while (1) {
    char* next_ptr = read_buffer + n_bytes_read;
    size_t remaining_capacity = BUFFER_SIZE - 1 - n_bytes_read;
    if (remaining_capacity <= 0) {
      break;
    }

    ssize_t n_bytes_read_this_time = read(conn->client_socket, next_ptr, remaining_capacity);
    if (n_bytes_read_this_time == 0) {
      DEBUG_LOG(
          "client %s closed the connection before sending full http CONNECT message, received %d bytes: %s",
          conn->client_hostport,
          n_bytes_read,
          read_buffer);
      return -1;
    } else if (n_bytes_read_this_time < 0) {
      if (errno == EINTR) {
        continue;  // interrupted by signal, try again
      }

      char* errno_desc = errno2s(errno);
      DEBUG_LOG(
          "reading for CONNECT from %s failed: %s, received %d bytes: %s",
          conn->client_hostport,
          errno_desc,
          n_bytes_read,
          read_buffer);
      free(errno_desc);
      return -1;
    }

    n_bytes_read += n_bytes_read_this_time;
    read_buffer[n_bytes_read] = '\0';

    char* double_crlf = strstr(read_buffer, "\r\n\r\n");
    if (double_crlf != NULL) {
      // received full CONNECT message
      if (parse_http_connect_message(read_buffer, conn) < 0) {
        // malformed CONNECT
        DEBUG_LOG("couldn't parse CONNECT message: %s", read_buffer);
        return -1;
      }
      char* next_char = double_crlf + 4;  // skip over the double crlf
      *read_buffer_ptr = next_char;
      return n_bytes_read - (next_char - read_buffer);
    }
  }

  DEBUG_LOG("no CONNECT message until buffer is full, abort");
  return -1;
}

void relay_sockets(struct tunnel_conn* conn, bool client_to_target) {
  int read_sock = client_to_target ? conn->client_socket : conn->target_socket;
  int write_sock = client_to_target ? conn->target_socket : conn->client_socket;
  const char* source_hostport = client_to_target ? conn->client_hostport : conn->target_hostport;
  const char* dest_hostport = client_to_target ? conn->target_hostport : conn->client_hostport;
  char* buffer = client_to_target ? conn->client_to_target_buffer : conn->target_to_client_buffer;

  DEBUG_LOG("relaying data (%s) -> (%s)", source_hostport, dest_hostport);

  while (1) {
    ssize_t bytes_read = read(read_sock, buffer, BUFFER_SIZE);
    if (bytes_read <= 0) {
      if (shutdown(write_sock, SHUT_RDWR) < 0) {
        perror("close after one end closes");
      }
      break;
    }
    DEBUG_LOG("received %zu bytes (%s) -> (%s)", bytes_read, source_hostport, dest_hostport);
    ssize_t bytes_written = write(write_sock, buffer, bytes_read);
    if (bytes_written < 0) {
      break;
    }
    DEBUG_LOG("wrote %zu bytes (%s) -> (%s)", bytes_written, source_hostport, dest_hostport);
  }
  DEBUG_LOG("stopping relay (%s) -> (%s)", source_hostport, dest_hostport);
}

void* relay_sockets_target_to_client_thread_func_wrapper(void* args) {
  struct tunnel_conn* conn = args;
  relay_sockets(conn, false);
  pthread_exit(NULL);
}

void handle_new_connection(struct tunnel_conn* conn) {
  char* next_char;
  ssize_t bytes_remaining = handle_http_connect_request(conn, &next_char);
  if (bytes_remaining < 0) {
    return;
  }

  DEBUG_LOG("received CONNECT request: %s %s:%s", conn->http_version, conn->target_host, conn->target_port);

  int target_socket = tcp_connect_to_target(conn);
  if (target_socket < 0) {
    send_unsuccessful_connect_response(conn);
    DEBUG_LOG("failed to connect to target %s", conn->target_hostport);
    return;
  }
  conn->target_socket = target_socket;
  DEBUG_LOG("connected to %s", conn->target_hostport);

  send_successful_connect_response(conn);

  // send the left-over bytes we read from the client to the server
  if (bytes_remaining > 0) {
    DEBUG_LOG("sending %d left over bytes after CONNECT", bytes_remaining);
    if (write(target_socket, next_char, bytes_remaining) < 0) {
      DEBUG_LOG("failed to send left over bytes from CONNECT");
      return;
    }
  }

  pthread_t worker;
  pthread_create(&worker, NULL, relay_sockets_target_to_client_thread_func_wrapper, conn);
  relay_sockets(conn, true);
  pthread_join(worker, NULL);

  DEBUG_LOG("tunnel (%s) -> (%s) closed", conn->client_hostport, conn->target_hostport);
}

void* handle_new_connection_thread_func_wrapper(void* args) {
  struct tunnel_conn* conn = args;
  handle_new_connection(conn);
  free_conn(conn);
  pthread_exit(NULL);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    die("provide listen port number as the first argument");
  }

  unsigned short listening_port;
  if (parse_port_number(argv[1], &listening_port) < 0) {
    die(hsprintf("failed to parse port number '%s'", argv[1]));
  }

  int listening_socket = create_bind_listen(listening_port);
  printf("Listening on port %hu\n", listening_port);

  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    die(hsprintf("failed to create epoll instance: %s", errno2s(errno)));
  }

  struct epoll_event event, events[EPOLL_MAX_EVENTS];

  event.data.fd = listening_socket;
  event.events = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
  // TODO: when multithreading, test whether thundering herd will occur
  // TODO: to distribute the new connections to multiple threads, we may need to use LT instead of ET
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listening_socket, &event) < 0) {
    die(hsprintf("failed to add listening socket into epoll: %s", errno2s(errno)));
  }

  while (1) {
    int num_events = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, -1);
    DEBUG_LOG("epoll_wait returned %d", num_events);
    if (num_events < 0) {
      die(hsprintf("epoll_wait error: %s", errno2s(errno)));
      break;
    }

    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    while (1) {
      // TODO: aceept as non-blocking sockets
      int client_socket = accept(listening_socket, (struct sockaddr*)&client_addr, &addrlen);
      if (client_socket < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // processed all incoming connections
          break;
        } else if (errno == EINTR) {
          // interrupted by signal, retry
          continue;
        } else {
          // unexpected error in accepting the connection
          die(hsprintf("accept4 failed: %s", errno2s(errno)));
        }
      }

      struct tunnel_conn* conn = init_conn();
      memcpy(conn->client_addr, &client_addr, addrlen);
      conn->client_socket = client_socket;
      set_client_hostport(conn);

      DEBUG_LOG("Received connection from %s", conn->client_hostport);

      pthread_t worker;
      pthread_create(&worker, NULL, handle_new_connection_thread_func_wrapper, conn);
    }
  }

  if (close(listening_socket) < 0) {
    die(hsprintf("failed to close listening socket: %s", errno2s(errno)));
  }

  if (close(epoll_fd) < 0) {
    die(hsprintf("failed to close epoll instance: %s", errno2s(errno)));
  }

  return 0;
}