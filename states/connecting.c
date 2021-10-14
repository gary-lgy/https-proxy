#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "../http.h"
#include "../log.h"
#include "../util.h"
#include "epoll_cb.h"

void fail_connecting_cb(int epoll_fd, struct epoll_connecting_cb* cb) {
  cb->failed = true;
  int n_bytes = sprintf(cb->conn->target_to_client_buffer.start, "%s 400 Bad Request \r\n\r\n", cb->conn->http_version);
  cb->conn->target_to_client_buffer.empty += n_bytes;

  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.ptr = cb;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, cb->conn->client_socket, &event) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add client_socket of %s to epoll for writing 400 response: %s",
        cb->conn->client_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(cb->conn);
    free(cb);
  }
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
  DEBUG_LOG("failed to connect to target %s: no more addresses to try", cb->conn->target_hostport);
  fail_connecting_cb(epoll_fd, cb);
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

void enter_connecting_state(int epoll_fd, struct tunnel_conn* conn) {
  struct epoll_connecting_cb* cb = malloc(sizeof(struct epoll_connecting_cb));
  cb->type = cb_type_connecting;
  cb->conn = conn;
  cb->failed = false;

  if (lookup_host_addr(conn->target_host, conn->target_port, &cb->host_addrs) < 0) {
    fail_connecting_cb(epoll_fd, cb);
    return;
  }
  cb->next_addr = cb->host_addrs;

  init_connection_to_target(epoll_fd, cb);
}

void handle_failed_connecting_cb(int epoll_fd, struct epoll_connecting_cb* cb) {
  struct tunnel_buffer* buf = &cb->conn->target_to_client_buffer;
  size_t n_bytes_to_send = buf->empty - buf->consumable;

  if (n_bytes_to_send <= 0) {
    die(hsprintf(
        "going to send 400 response for tunnel (%s) -> (%s), but the buf is empty; this should not happen",
        cb->conn->client_hostport,
        cb->conn->target_hostport));
  }

  ssize_t n_bytes_sent = send(cb->conn->client_socket, buf->consumable, n_bytes_to_send, MSG_NOSIGNAL);

  if (n_bytes_sent < 0) {
    // teardown the entire connection
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to write 400 response for (%s) -> (%s): %s",
        cb->conn->client_hostport,
        cb->conn->target_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(cb->conn);
    free(cb);
    return;
  }

  DEBUG_LOG(
      "sent %d bytes of 400 response to client of (%s) -> (%s)",
      n_bytes_sent,
      cb->conn->client_hostport,
      cb->conn->target_hostport);

  buf->consumable += n_bytes_sent;

  if (buf->consumable >= buf->empty) {
    // all bytes sent
    destroy_tunnel_conn(cb->conn);
    free(cb);
    return;
  }

  // still some bytes left, wait to send again
  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.ptr = cb;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, cb->conn->client_socket, &event) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add client_socket of %s to epoll for writing 400 response: %s",
        cb->conn->client_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(cb->conn);
    free(cb);
  }
}

void handle_connecting_cb(int epoll_fd, struct epoll_connecting_cb* cb, uint32_t events) {
  if (events & EPOLLERR) {
    DEBUG_LOG(
        "epoll reported error on tunnel connection (%s) -> (%s) in connecting state",
        cb->conn->client_hostport,
        cb->conn->target_hostport);
  }

  // after connection failure, we send 400 response to client
  if (cb->failed) {
    handle_failed_connecting_cb(epoll_fd, cb);
    return;
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
