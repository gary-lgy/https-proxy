#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "epoll_cb.h"
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

int lookup_host_addr(const char* hostname, const char* port, struct addrinfo** results) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  // TODO: use getaddrinfo_a and signalfd to achieve non-blocking dns resolution
  int gai_errno = getaddrinfo(hostname, port, &hints, results);
  if (gai_errno != 0) {
    DEBUG_LOG(hsprintf("target_host resolution failed: %s", gai_strerror(gai_errno)));
    return -1;
  }

  return 0;
}

void init_connection_to_target(int epoll_fd, struct epoll_connecting_cb* cb) {
  for (; cb->next_addr != NULL; cb->next_addr = cb->next_addr->ai_next) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (sock < 0) {
      continue;
    }

    while (1) {
      if (connect(sock, cb->next_addr->ai_addr, sizeof(struct sockaddr_in)) == 0 || errno == EAGAIN ||
          errno == EINPROGRESS) {
        // we're connected or connecting to the current address
        cb->next_addr = cb->next_addr->ai_next;
        cb->sock = sock;

        struct epoll_event event;
        event.events = EPOLLOUT | EPOLLONESHOT;
        event.data.ptr = cb;
        // TODO: check return values of epoll_ctl
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cb->sock, &event);
        return;
      }

      if (errno == EINTR) {
        // interrupted, try again
        continue;
      }

      // some unexpected error
      close(sock);
      break;
    }
  }

  // none of the addresses work
  DEBUG_LOG("failed to connect to target %s", cb->conn->target_hostport);
  send_unsuccessful_connect_response(cb->conn);
  freeaddrinfo(cb->host_addrs);
  free_conn(cb->conn);
  free(cb);
}

void enter_connecting_state(int epoll_fd, struct tunnel_conn* conn) {
  // TODO: send 400 asynchronously
  struct epoll_connecting_cb* cb = malloc(sizeof(struct epoll_connecting_cb));
  cb->type = cb_type_connecting;
  cb->conn = conn;

  if (lookup_host_addr(conn->target_host, conn->target_port, &cb->host_addrs) < 0) {
    send_unsuccessful_connect_response(conn);
    free(cb);
    free_conn(conn);
    return;
  }
  cb->next_addr = cb->host_addrs;

  init_connection_to_target(epoll_fd, cb);
}

ssize_t read_into_buffer(int read_fd, struct tunnel_buffer* buf) {
  buf->empty[0] = '\0';

  while (1) {
    size_t remaining_capacity = BUFFER_SIZE - 1 - (buf->empty - buf->start);
    if (remaining_capacity <= 0) {
      return -2;
    }

    ssize_t n_bytes_read = read(read_fd, buf->empty, remaining_capacity);
    if (n_bytes_read == -1 && errno == EINTR) {
      // interrupted by signal, try again
      continue;
    }
    if (n_bytes_read <= 0) {
      return n_bytes_read;
    }

    buf->empty += n_bytes_read;
    buf->empty[0] = '\0';
    return n_bytes_read;
  }
}

/**
 * @param conn
 * @return -1 if an error occurred and conn should be closed; 0 if CONNECT was found and parsed; 1 if we need more
 * bytes.
 */
int find_and_parse_http_connect(struct tunnel_conn* conn) {
  struct tunnel_buffer* buf = &conn->client_to_target_buffer;
  ssize_t n_bytes_read = read_into_buffer(conn->client_socket, buf);

  if (n_bytes_read == 0) {
    DEBUG_LOG(
        "client %s closed the connection before sending full http CONNECT message, received %d bytes: %s",
        conn->client_hostport,
        buf->empty - buf->start,
        buf->start);
    return -1;
  } else if (n_bytes_read < 0) {
    char* errno_desc = errno2s(errno);
    DEBUG_LOG(
        "reading for CONNECT from %s failed: %s, received %d bytes: %s",
        conn->client_hostport,
        errno_desc,
        buf->empty - buf->start,
        buf->start);
    free(errno_desc);
    return -1;
  }

  char* double_crlf = strstr(buf->start, "\r\n\r\n");
  if (double_crlf != NULL) {
    // received full CONNECT message
    char *host, *port, *http_version;
    if (parse_http_connect_message(buf->start, &host, &port, &http_version) < 0) {
      // malformed CONNECT
      DEBUG_LOG("couldn't parse CONNECT message: %s", buf->start);
      return -1;
    }

    strncpy(conn->target_host, host, MAX_HOST_LEN);
    strncpy(conn->target_port, port, MAX_PORT_LEN);
    strncpy(conn->http_version, http_version, HTTP_VERSION_LEN);

    set_target_hostport(conn);

    buf->consumable = double_crlf + 4;  // skip over the double crlf

    DEBUG_LOG("received CONNECT request: %s %s:%s", conn->http_version, conn->target_host, conn->target_port);

    return 0;
  }

  // we don't have an HTTP message yet, can we read more bytes?

  if (buf->empty >= buf->start + BUFFER_SIZE - 1) {
    // no, the buffer is full
    DEBUG_LOG("no CONNECT message from %s until buffer is full, buffer content: %s", conn->client_hostport, buf->start);
    return -1;
  }

  // let's read more bytes
  return 1;
}

void relay_sockets(struct tunnel_conn* conn, bool client_to_target) {
  // TODO: shift buffer pointers in case of partial writes
  int read_sock = client_to_target ? conn->client_socket : conn->target_socket;
  int write_sock = client_to_target ? conn->target_socket : conn->client_socket;
  const char* source_hostport = client_to_target ? conn->client_hostport : conn->target_hostport;
  const char* dest_hostport = client_to_target ? conn->target_hostport : conn->client_hostport;
  struct tunnel_buffer* buffer = client_to_target ? &conn->client_to_target_buffer : &conn->target_to_client_buffer;

  DEBUG_LOG("relaying data (%s) -> (%s)", source_hostport, dest_hostport);

  while (1) {
    ssize_t bytes_read = read(read_sock, buffer->start, BUFFER_SIZE);
    if (bytes_read <= 0) {
      if (shutdown(write_sock, SHUT_RDWR) < 0) {
        perror("close after one end closes");
      }
      break;
    }
    DEBUG_LOG("received %zu bytes (%s) -> (%s)", bytes_read, source_hostport, dest_hostport);
    ssize_t bytes_written = write(write_sock, buffer->start, bytes_read);
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
  send_successful_connect_response(conn);

  // send the left-over bytes we read from the client to the server
  size_t n_bytes_remaining = conn->client_to_target_buffer.empty - conn->client_to_target_buffer.consumable;
  if (n_bytes_remaining > 0) {
    DEBUG_LOG("sending %d left over bytes after CONNECT", n_bytes_remaining);
    if (write(conn->target_socket, conn->client_to_target_buffer.consumable, n_bytes_remaining) < 0) {
      DEBUG_LOG("failed to send left over bytes from CONNECT");
      return;
    }
    // reset the buffer
    conn->client_to_target_buffer.consumable = conn->client_to_target_buffer.start;
    conn->client_to_target_buffer.empty = conn->client_to_target_buffer.start;
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

void accept_incoming_connections(int epoll_fd, int listening_socket) {
  while (1) {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(struct sockaddr_in);

    // TODO: accept as non-blocking sockets?
    int client_socket = accept(listening_socket, (struct sockaddr*)&client_addr, &addrlen);
    if (client_socket < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // processed all incoming connections
        return;
      } else if (errno == EINTR) {
        // interrupted by signal, retry
        continue;
      } else {
        // unexpected error in accepting the connection
        char* error_desc = errno2s(errno);
        DEBUG_LOG("accept failed: %s", error_desc);
        free(error_desc);
        return;
      }
    }

    struct tunnel_conn* conn = init_conn();
    memcpy(conn->client_addr, &client_addr, addrlen);
    conn->client_socket = client_socket;
    set_client_hostport(conn);

    DEBUG_LOG("Received connection from %s", conn->client_hostport);

    // add client socket to epoll and wait for readability
    struct epoll_accepted_cb* cb = malloc(sizeof(struct epoll_accepted_cb));
    cb->type = cb_type_accepted;
    cb->conn = conn;

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = cb;
    // TODO: check return value
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event);
  }
}

void handle_accepted_cb(int epoll_fd, struct epoll_accepted_cb* cb, uint32_t events) {
  struct tunnel_conn* conn = cb->conn;

  if (events & EPOLLERR) {
    DEBUG_LOG(
        "epoll reported error on tunnel connection (%s) -> (%s) in accepted state",
        conn->client_hostport,
        conn->target_hostport);
  }

  int result = find_and_parse_http_connect(conn);
  if (result < 0) {
    free_conn(conn);
    free(cb);
  } else if (result == 0) {
    // try connecting to target
    free(cb);
    enter_connecting_state(epoll_fd, conn);
  } else {
    // need to read more bytes, wait for readability again
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = cb;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->client_socket, &event);
  }
}

void handle_connecting_cb(int epoll_fd, struct epoll_connecting_cb* cb, uint32_t events) {
  if (events & EPOLLERR) {
    DEBUG_LOG(
        "epoll reported error on tunnel connection (%s) -> (%s) in connecting state",
        cb->conn->client_hostport,
        cb->conn->target_hostport);
  }

  // Check if the connection succeeded
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  if (getpeername(cb->sock, &addr, &addrlen) < 0) {
    // connection failed; try connecting with another address
    shutdown(cb->sock, SHUT_RDWR);
    close(cb->sock);
    init_connection_to_target(epoll_fd, cb);
  } else {
    // connection succeeded
    cb->conn->target_socket = cb->sock;
    memcpy(cb->conn->target_addr, &addr, sizeof(struct sockaddr_in));
    DEBUG_LOG("connected to %s", cb->conn->target_hostport);

    // TODO: examine whether we should use non blocking sockets
    const int flags = fcntl(cb->sock, F_GETFL, 0) & (~O_NONBLOCK);
    fcntl(cb->sock, F_SETFL, flags);

    pthread_t worker;
    pthread_create(&worker, NULL, handle_new_connection_thread_func_wrapper, cb->conn);

    freeaddrinfo(cb->host_addrs);
    free(cb);
  }
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

  // NULL means we have events on listening socket
  event.data.ptr = NULL;
  event.events = EPOLLIN;
  // TODO: when multithreading, test whether thundering herd will occur
  // TODO: to distribute the new connections to multiple threads, we may need to use LT instead of ET
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listening_socket, &event) < 0) {
    die(hsprintf("failed to add listening socket into epoll: %s", errno2s(errno)));
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
        accept_incoming_connections(epoll_fd, listening_socket);
      } else {
        // events on existing connection
        struct epoll_cb* cb = events[i].data.ptr;
        if (cb->type == cb_type_accepted) {
          handle_accepted_cb(epoll_fd, (struct epoll_accepted_cb*)cb, events[i].events);
        } else if (cb->type == cb_type_connecting) {
          handle_connecting_cb(epoll_fd, (struct epoll_connecting_cb*)cb, events[i].events);
        }
      }
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