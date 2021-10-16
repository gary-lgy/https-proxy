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
#include "../lib/asyncaddrinfo/asyncaddrinfo.h"
#include "../log.h"
#include "../util.h"
#include "epoll_cb.h"

void fail_connecting_cb(int epoll_fd, struct epoll_connecting_cb* cb) {
  cb->failed = true;

  // Send HTTP 4xx to client
  int n_bytes = sprintf(cb->conn->target_to_client_buffer.start, "%s 400 Bad Request \r\n\r\n", cb->conn->http_version);
  cb->conn->target_to_client_buffer.write_ptr += n_bytes;

  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.ptr = cb;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, cb->conn->client_socket, &event) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add client_socket of %s to epoll for writing 4xx response: %s",
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
      cb->target_conn_sock = sock;

      struct epoll_event event;
      event.events = EPOLLOUT | EPOLLONESHOT;
      event.data.ptr = cb;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cb->target_conn_sock, &event) < 0) {
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
  freeaddrinfo(cb->host_addrs);
  fail_connecting_cb(epoll_fd, cb);
}

int submit_hostname_lookup(int epoll_fd, struct epoll_connecting_cb* cb, const char* hostname, const char* port) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  cb->asyncaddrinfo_fd = asyncaddrinfo_resolve(hostname, port, &hints);

  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.ptr = cb;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cb->asyncaddrinfo_fd, &event) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add asyncaddrinfo_fd for (%s) -> (%s) into epoll: %s",
        cb->conn->client_hostport,
        cb->conn->target_hostport,
        error_desc);
    free(error_desc);
    close(cb->asyncaddrinfo_fd);
    return -1;
  }

  return 0;
}

void enter_connecting_state(int epoll_fd, struct tunnel_conn* conn) {
  struct epoll_connecting_cb* cb = malloc(sizeof(struct epoll_connecting_cb));
  cb->type = cb_type_connecting;
  cb->conn = conn;
  cb->failed = false;

  // Check blacklist
  char** blacklist = conn->blacklist;
  int blacklist_len = conn->blacklist_len;
  for (int i = 0; i < blacklist_len; i++) {
    if (strstr(conn->target_host, blacklist[i]) != NULL) {
      conn->is_blocked = true;
      fail_connecting_cb(epoll_fd, cb);
      DEBUG_LOG(
        "block target: '%s' as it matches '%s'",
        cb->conn->target_host,
        blacklist[i]);
      return;
    }
  }

  if (submit_hostname_lookup(epoll_fd, cb, conn->target_host, conn->target_port) < 0) {
    fail_connecting_cb(epoll_fd, cb);
    return;
  }
}

void handle_failed_connecting_cb(int epoll_fd, struct epoll_connecting_cb* cb) {
  struct tunnel_buffer* buf = &cb->conn->target_to_client_buffer;
  size_t n_bytes_to_send = buf->write_ptr - buf->read_ptr;

  if (n_bytes_to_send <= 0) {
    die(hsprintf(
        "going to send 4xx response for tunnel (%s) -> (%s), but the buf is empty; this should not happen",
        cb->conn->client_hostport,
        cb->conn->target_hostport));
  }

  ssize_t n_bytes_sent = send(cb->conn->client_socket, buf->read_ptr, n_bytes_to_send, MSG_NOSIGNAL);

  if (n_bytes_sent < 0) {
    // teardown the entire connection
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to write 4xx response for (%s) -> (%s): %s",
        cb->conn->client_hostport,
        cb->conn->target_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(cb->conn);
    free(cb);
    return;
  }

  DEBUG_LOG(
      "sent %d bytes of 4xx response to client of (%s) -> (%s)",
      n_bytes_sent,
      cb->conn->client_hostport,
      cb->conn->target_hostport);

  buf->read_ptr += n_bytes_sent;

  if (buf->read_ptr >= buf->write_ptr) {
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
        "failed to add client_socket of %s to epoll for writing 4xx response: %s",
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

  // after connection failure, we send 4xx response to client
  if (cb->failed) {
    handle_failed_connecting_cb(epoll_fd, cb);
    return;
  }

  // we're either waiting for asyncaddrinfo lookup result or connection to target to complete
  // for the former, we are waiting for EPOLLIN, and for the latter, we are waiting for EPOLLOUT
  if (events & EPOLLIN) {
    // asyncaddrinfo result came back
    int gai_errno = asyncaddrinfo_result(cb->asyncaddrinfo_fd, &cb->host_addrs);
    if (gai_errno != 0) {
      DEBUG_LOG(
          "host resolution for (%s) -> (%s) failed: %s",
          cb->conn->client_hostport,
          cb->conn->target_hostport,
          gai_strerror(gai_errno));
      fail_connecting_cb(epoll_fd, cb);
      return;
    }
    cb->asyncaddrinfo_fd = -1;

    DEBUG_LOG("host resolution succeeded for (%s) -> (%s)", cb->conn->client_hostport, cb->conn->target_hostport);

    // start connecting
    cb->next_addr = cb->host_addrs;
    init_connection_to_target(epoll_fd, cb);
  } else {
    // connection succeeded or failed
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(cb->target_conn_sock, &addr, &addrlen) < 0) {
      // connection failed; try connecting with another address
      shutdown(cb->target_conn_sock, SHUT_RDWR);
      close(cb->target_conn_sock);
      init_connection_to_target(epoll_fd, cb);
    } else {
      // connection succeeded
      cb->conn->target_socket = cb->target_conn_sock;
      DEBUG_LOG("connected to %s", cb->conn->target_hostport);

      enter_tunneling_state(epoll_fd, cb->conn);

      freeaddrinfo(cb->host_addrs);
      free(cb);
    }
  }
}
