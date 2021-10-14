#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
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

    if (connect(sock, cb->next_addr->ai_addr, sizeof(struct sockaddr_in)) == 0 || errno == EAGAIN ||
        errno == EINPROGRESS) {
      // we're connected or connecting to the current address
      cb->next_addr = cb->next_addr->ai_next;
      cb->sock = sock;

      struct epoll_event event;
      event.events = EPOLLOUT | EPOLLONESHOT;
      event.data.ptr = cb;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cb->sock, &event) < 0) {
        char* error_desc = errno2s(errno);
        DEBUG_LOG("failed to add target socket into epoll: %s", error_desc);
        free(error_desc);

        freeaddrinfo(cb->host_addrs);
        destroy_tunnel_conn(cb->conn);
        close(sock);
        free(cb);
      }
      return;
    }

    // some unexpected error, try the next address
    close(sock);
  }

  // none of the addresses work
  DEBUG_LOG("failed to connect to target %s", cb->conn->target_hostport);
  send_unsuccessful_connect_response(cb->conn);
  freeaddrinfo(cb->host_addrs);
  destroy_tunnel_conn(cb->conn);
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
    destroy_tunnel_conn(conn);
    return;
  }
  cb->next_addr = cb->host_addrs;

  init_connection_to_target(epoll_fd, cb);
}

ssize_t read_into_buffer(int read_fd, struct tunnel_buffer* buf) {
  buf->empty[0] = '\0';

  size_t remaining_capacity = BUFFER_SIZE - 1 - (buf->empty - buf->start);
  if (remaining_capacity <= 0) {
    return -2;
  }

  ssize_t n_bytes_read = read(read_fd, buf->empty, remaining_capacity);

  if (n_bytes_read <= 0) {
    return n_bytes_read;
  }

  buf->empty += n_bytes_read;
  buf->empty[0] = '\0';
  return n_bytes_read;
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

void accept_incoming_connections(int epoll_fd, int listening_socket, bool telemetry_enabled) {
  while (1) {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(struct sockaddr_in);

    int client_socket = accept4(listening_socket, (struct sockaddr*)&client_addr, &addrlen, SOCK_NONBLOCK);
    if (client_socket < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // processed all incoming connections
        return;
      } else {
        // unexpected error in accepting the connection
        char* error_desc = errno2s(errno);
        DEBUG_LOG("accept failed: %s", error_desc);
        free(error_desc);
        return;
      }
    }

    struct tunnel_conn* conn = create_tunnel_conn(telemetry_enabled);
    conn->client_socket = client_socket;
    set_client_hostport(conn, &client_addr);

    DEBUG_LOG("Received connection from %s", conn->client_hostport);

    // add client socket to epoll and wait for readability
    struct epoll_accepted_cb* cb = malloc(sizeof(struct epoll_accepted_cb));
    cb->type = cb_type_accepted;
    cb->conn = conn;

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = cb;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG("failed to add accepted client socket from %s into epoll: %s", conn->client_hostport, error_desc);
      free(error_desc);
      destroy_tunnel_conn(conn);
      free(cb);
    }
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
    destroy_tunnel_conn(conn);
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
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->client_socket, &event) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG("failed to re-add client socket from %s for reading CONNECT: %s", conn->client_hostport, error_desc);
      free(error_desc);

      destroy_tunnel_conn(conn);
      free(cb);
    }
  }
}

void enter_tunneling_state(int epoll_fd, struct tunnel_conn* conn) {
  // dup each socket to decouple read and write ends of the socket
  // this allows us to set different event masks on the same socket
  int client_read_fd = conn->client_socket;
  int client_write_fd = conn->client_socket_dup = dup(client_read_fd);
  int target_read_fd = conn->target_socket;
  int target_write_fd = conn->target_socket_dup = dup(target_read_fd);

  // During tunneling, we will switch to epoll_wait on all these FDs.
  // If the FD has already been added, EPOLL_CTL_ADD will not work; only EPOLL_CTL_MOD can reactivate the FD.
  // On the other hand, EPOLL_CTL_MOD will only work for FDs that have not been added.
  // However, we have no idea which FDs are in the epoll instance already.
  // For convenience, we make sure all FDs have been added to the epoll instance.
  // Hence, we can use EPOLL_CTL_MOD later when we want to wait on the socket again.
  struct epoll_event event;
  event.events = EPOLLERR | EPOLLONESHOT;
  event.data.ptr = NULL;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, target_write_fd, &event)) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add target_write_fd %d for (%s) -> (%s) into epoll: %s",
        target_write_fd,
        conn->client_hostport,
        conn->target_hostport,
        error_desc);
    free(error_desc);
    destroy_tunnel_conn(conn);
    return;
  }

  // TODO: free memory / close dup when cleaning up after error
  // Send HTTP 200 to client
  int n_bytes =
      sprintf(conn->target_to_client_buffer.start, "%s 200 Connection Established \r\n\r\n", conn->http_version);
  conn->target_to_client_buffer.empty += n_bytes;

  struct epoll_tunneling_cb* cb = malloc(sizeof(struct epoll_tunneling_cb));
  cb->type = cb_type_tunneling;
  cb->conn = conn;
  cb->is_client_to_target = false;
  cb->is_read = false;

  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.ptr = cb;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_write_fd, &event) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add client socket for (%s) -> (%s) to epoll for writing 200 OK message: %s",
        conn->client_hostport,
        conn->target_hostport,
        error_desc);
    free(error_desc);

    free(cb);
    destroy_tunnel_conn(conn);
    return;
  }

  size_t n_bytes_remaining = conn->client_to_target_buffer.empty - conn->client_to_target_buffer.consumable;
  cb = malloc(sizeof(struct epoll_tunneling_cb));
  if (n_bytes_remaining > 0) {
    // if we received more than just the CONNECT message from the client, send the rest of the bytes to the target
    DEBUG_LOG("sending %d left over bytes after CONNECT", n_bytes_remaining);
    cb->type = cb_type_tunneling;
    cb->conn = conn;
    cb->is_client_to_target = true;
    cb->is_read = false;

    event.events = EPOLLOUT | EPOLLONESHOT;
    event.data.ptr = cb;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, target_write_fd, &event) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG(
          "failed to add target socket for (%s) -> (%s) to epoll for bytes after CONNECT: %s",
          conn->client_hostport,
          conn->target_hostport,
          error_desc);
      free(error_desc);

      free(cb);
      destroy_tunnel_conn(conn);
      return;
    }
  } else {
    // wait to read from client

    // reset the buffer
    conn->client_to_target_buffer.consumable = conn->client_to_target_buffer.start;
    conn->client_to_target_buffer.empty = conn->client_to_target_buffer.start;

    cb = malloc(sizeof(struct epoll_tunneling_cb));
    cb->type = cb_type_tunneling;
    cb->conn = conn;
    cb->is_client_to_target = true;
    cb->is_read = true;

    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = cb;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_read_fd, &event) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG(
          "failed to add client socket for (%s) -> (%s) to epoll for reading: %s",
          conn->client_hostport,
          conn->target_hostport,
          error_desc);
      free(error_desc);

      free(cb);
      destroy_tunnel_conn(conn);
      return;
    }
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
    DEBUG_LOG("connected to %s", cb->conn->target_hostport);

    enter_tunneling_state(epoll_fd, cb->conn);

    freeaddrinfo(cb->host_addrs);
    free(cb);
  }
}

void handle_tunneling_read(
    int epoll_fd,
    struct epoll_tunneling_cb* cb,
    const char* source_hostport,
    const char* dest_hostport,
    struct tunnel_buffer* buf,
    int polled_fd,
    int opposite_fd) {
  size_t remaining_capacity = BUFFER_SIZE - (buf->empty - buf->start);
  if (remaining_capacity <= 0) {
    die(hsprintf(
        "going to read for tunnel (%s) -> (%s), but the buf is full; this should not happen",
        source_hostport,
        dest_hostport));
  }

  ssize_t n_bytes_read = read(polled_fd, buf->empty, remaining_capacity);

  if (n_bytes_read == 0) {
    // peer stopped sending
    DEBUG_LOG("peer (%s) -> (%s) closed connection", source_hostport, dest_hostport);
    shutdown(polled_fd, SHUT_RD);
    shutdown(opposite_fd, SHUT_WR);
    if (++cb->conn->halves_closed == 2) {
      DEBUG_LOG("tunnel (%s) -> (%s) closed", cb->conn->client_hostport, cb->conn->target_hostport);
      // both halves closed, tear down the whole connection
      destroy_tunnel_conn(cb->conn);
      free(cb);
    }
    return;
  } else if (n_bytes_read < 0) {
    // read error
    char* error_desc = errno2s(errno);
    DEBUG_LOG("read error from (%s) -> (%s): %s", source_hostport, dest_hostport, error_desc);
    free(error_desc);
    destroy_tunnel_conn(cb->conn);
    free(cb);
    return;
  }

  DEBUG_LOG("received %zu bytes (%s) -> (%s)", n_bytes_read, source_hostport, dest_hostport);
  if (!cb->is_client_to_target) {
    cb->conn->n_bytes_streamed += n_bytes_read;
  }

  buf->empty += n_bytes_read;

  cb->is_read = false;

  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.ptr = cb;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, opposite_fd, &event) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add writing socket for (%s) -> (%s) for tunneling: %s", source_hostport, dest_hostport, error_desc);
    free(error_desc);

    destroy_tunnel_conn(cb->conn);
    free(cb);
  }
}

void handle_tunneling_write(
    int epoll_fd,
    struct epoll_tunneling_cb* cb,
    const char* source_hostport,
    const char* dest_hostport,
    struct tunnel_buffer* buf,
    int polled_fd,
    int opposite_fd) {
  size_t n_bytes_to_send = buf->empty - buf->consumable;

  if (n_bytes_to_send <= 0) {
    die(hsprintf(
        "going to write for tunnel (%s) -> (%s), but the buf is empty; this should not happen",
        source_hostport,
        dest_hostport));
  }

  ssize_t n_bytes_sent = send(polled_fd, buf->consumable, n_bytes_to_send, MSG_NOSIGNAL);

  if (n_bytes_sent < 0) {
    // peer refused to receive?
    // teardown the entire connection
    char* error_desc = errno2s(errno);
    DEBUG_LOG("write error from (%s) -> (%s): %s", source_hostport, dest_hostport, error_desc);
    free(error_desc);

    destroy_tunnel_conn(cb->conn);
    free(cb);
    return;
  }

  DEBUG_LOG("wrote %zu bytes (%s) -> (%s)", n_bytes_sent, source_hostport, dest_hostport);

  buf->consumable += n_bytes_sent;

  struct epoll_event event;
  if (buf->consumable >= buf->empty) {
    // sent everything, we can read again
    buf->consumable = buf->empty = buf->start;

    cb->is_read = true;
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = cb;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, opposite_fd, &event) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG(
          "failed to add reading socket for (%s) -> (%s) for tunneling: %s",
          source_hostport,
          dest_hostport,
          error_desc);
      free(error_desc);

      destroy_tunnel_conn(cb->conn);
      free(cb);
    }
  } else {
    // wait for writability to send again later
    event.events = EPOLLOUT | EPOLLONESHOT;
    event.data.ptr = cb;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, polled_fd, &event) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG(
          "failed to add writing socket for (%s) -> (%s) for tunneling: %s",
          source_hostport,
          dest_hostport,
          error_desc);
      free(error_desc);

      destroy_tunnel_conn(cb->conn);
      free(cb);
    }
  }
}

void handle_tunneling_cb(int epoll_fd, struct epoll_tunneling_cb* cb, uint32_t events) {
  const char* source_hostport = cb->is_client_to_target ? cb->conn->client_hostport : cb->conn->target_hostport;
  const char* dest_hostport = cb->is_client_to_target ? cb->conn->target_hostport : cb->conn->client_hostport;
  struct tunnel_buffer* buf =
      cb->is_client_to_target ? &cb->conn->client_to_target_buffer : &cb->conn->target_to_client_buffer;

  int polled_fd, opposite_fd;
  if (cb->is_client_to_target) {
    if (cb->is_read) {
      polled_fd = cb->conn->client_socket;
      opposite_fd = cb->conn->target_socket_dup;
    } else {
      polled_fd = cb->conn->target_socket_dup;
      opposite_fd = cb->conn->client_socket;
    }
  } else {
    if (cb->is_read) {
      polled_fd = cb->conn->target_socket;
      opposite_fd = cb->conn->client_socket_dup;
    } else {
      polled_fd = cb->conn->client_socket_dup;
      opposite_fd = cb->conn->target_socket;
    }
  }

  if (events & EPOLLERR) {
    DEBUG_LOG(
        "epoll reported error on tunnel connection (%s) -> (%s) in tunneling state", source_hostport, dest_hostport);
  }

  if (events & EPOLLIN) {
    assert(cb->is_read);
    handle_tunneling_read(epoll_fd, cb, source_hostport, dest_hostport, buf, polled_fd, opposite_fd);
  } else if (events & EPOLLOUT) {
    assert(!cb->is_read);
    handle_tunneling_write(epoll_fd, cb, source_hostport, dest_hostport, buf, polled_fd, opposite_fd);
  }
}

int main(int argc, char** argv) {
  if (argc != 4) {
    die(hsprintf("Usage: %s <port> <flag_telemetry> <filename of blacklist>", argv[0]));
  }

  unsigned short listening_port;
  if (parse_port_number(argv[1], &listening_port) < 0) {
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
  //  const char* blacklist_filename = argv[3];

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
        if (!(events[i].events & EPOLLIN)) {
          DEBUG_LOG("listening socket is not readable but epoll woke us up anyway");
          continue;
        }
        accept_incoming_connections(epoll_fd, listening_socket, telemetry_enabled);
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

  if (close(listening_socket) < 0) {
    die(hsprintf("failed to close listening socket: %s", errno2s(errno)));
  }

  if (close(epoll_fd) < 0) {
    die(hsprintf("failed to close epoll instance: %s", errno2s(errno)));
  }

  return 0;
}