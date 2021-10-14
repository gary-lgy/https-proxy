#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "../http.h"
#include "../log.h"
#include "../util.h"
#include "epoll_cb.h"

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
