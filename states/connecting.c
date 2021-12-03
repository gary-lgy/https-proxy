#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../http.h"
#include "../lib/asyncaddrinfo/asyncaddrinfo.h"
#include "../log.h"
#include "../poll.h"
#include "../util.h"
#include "epoll_cb.h"

// Information for a connection that is in the process of connecting to the target
struct connecting_data_block {
  struct tunnel_conn* conn;
  int asyncaddrinfo_fd;
  struct addrinfo* host_addrs;
  struct addrinfo* next_addr;
  int target_sock;
};

void send_rejection_response_to_client(struct poll* p, struct tunnel_conn* conn);

void prepare_rejection_response(struct tunnel_conn* conn) {
  int n_bytes = sprintf(conn->to_client_buffer.start, "%s 400 Bad Request \r\n\r\n", conn->http_version);
  conn->to_client_buffer.write_ptr += n_bytes;
}

void wait_to_send_rejection_response_to_client(struct poll* p, struct tunnel_conn* conn) {
  if (poll_wait_for_writability(
          p, conn->client_socket, conn, true, false, (poll_callback)send_rejection_response_to_client) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add client_socket of %s to poll instance for writing 4xx response: %s",
        conn->client_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(conn);
  }
}

void send_rejection_response_to_client(struct poll* p, struct tunnel_conn* conn) {
  struct tunnel_buffer* buf = &conn->to_client_buffer;
  size_t n_bytes_to_send = buf->write_ptr - buf->read_ptr;

  if (n_bytes_to_send <= 0) {
    die(hsprintf(
        "going to send 4xx response for tunnel (%s) -> (%s), but the buf is empty; this should not happen",
        conn->client_hostport,
        conn->target_hostport));
  }

  ssize_t n_bytes_sent = send(conn->client_socket, buf->read_ptr, n_bytes_to_send, MSG_NOSIGNAL);

  if (n_bytes_sent < 0) {
    // teardown the entire connection
    char* error_desc = errno2s(errno);
    LOG("failed to write 4xx response for (%s) -> (%s): %s", conn->client_hostport, conn->target_hostport, error_desc);
    free(error_desc);

    destroy_tunnel_conn(conn);
    return;
  }

  DEBUG_LOG(
      "sent %d bytes of 4xx response to client of (%s) -> (%s)",
      n_bytes_sent,
      conn->client_hostport,
      conn->target_hostport);

  buf->read_ptr += n_bytes_sent;

  if (buf->read_ptr >= buf->write_ptr) {
    // all bytes sent
    destroy_tunnel_conn(conn);
  } else {
    // still some bytes left, wait to send again
    wait_to_send_rejection_response_to_client(p, conn);
  }
}

void reject_client_request(struct poll* p, struct tunnel_conn* conn) {
  prepare_rejection_response(conn);
  wait_to_send_rejection_response_to_client(p, conn);
}

void handle_connection_completed(struct poll* p, struct connecting_data_block* data_block);

void connect_to_target(struct poll* p, struct connecting_data_block* data_block) {
  // try all addresses
  for (; data_block->next_addr != NULL; data_block->next_addr = data_block->next_addr->ai_next) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (sock < 0) {
      continue;
    }

    if (connect(sock, data_block->next_addr->ai_addr, sizeof(struct sockaddr_in)) != 0 && errno != EAGAIN &&
        errno != EINPROGRESS) {
      // connect failed
      close(sock);
      continue;
    }

    data_block->target_sock = sock;

    // wait until the connection is successful by waiting for writability on the socket
    if (poll_wait_for_writability(p, sock, data_block, true, false, (poll_callback)handle_connection_completed) < 0) {
      // cannot add the socket to the poll instance for some reason
      char* error_desc = errno2s(errno);
      DEBUG_LOG("failed to add target socket into epoll: %s", error_desc);
      free(error_desc);

      close(sock);
      continue;
    }

    // we're connecting to the current address
    data_block->next_addr = data_block->next_addr->ai_next;
    return;
  }

  // none of the addresses work
  LOG("failed to connect to target %s: no more addresses to try", data_block->conn->target_hostport);
  freeaddrinfo(data_block->host_addrs);
  reject_client_request(p, data_block->conn);
  free(data_block);
}

void handle_connection_completed(struct poll* p, struct connecting_data_block* data_block) {
  // connection succeeded or failed
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  if (getpeername(data_block->target_sock, &addr, &addrlen) < 0) {
    // connection failed; try connecting with another address
    shutdown(data_block->target_sock, SHUT_RDWR);
    close(data_block->target_sock);
    connect_to_target(p, data_block);
  } else {
    // connection succeeded
    data_block->conn->target_socket = data_block->target_sock;
    LOG("connected to %s", data_block->conn->target_hostport);

    freeaddrinfo(data_block->host_addrs);
    start_tunneling(p, data_block->conn);
    free(data_block);
  }
}

void handle_asyncaddrinfo_resolve_readability(struct poll* p, struct connecting_data_block* data_block) {
  int gai_errno = asyncaddrinfo_result(data_block->asyncaddrinfo_fd, &data_block->host_addrs);
  if (gai_errno != 0) {
    LOG("host resolution for (%s) -> (%s) failed: %s",
        data_block->conn->client_hostport,
        data_block->conn->target_hostport,
        gai_strerror(gai_errno));
    reject_client_request(p, data_block->conn);
    free(data_block);
    return;
  }

  data_block->asyncaddrinfo_fd = -1;
  LOG("host resolution succeeded for (%s) -> (%s)",
      data_block->conn->client_hostport,
      data_block->conn->target_hostport);

  // start connecting
  data_block->next_addr = data_block->host_addrs;
  connect_to_target(p, data_block);
}

int submit_hostname_lookup(
    struct poll* p,
    struct connecting_data_block* data_block,
    const char* hostname,
    const char* port) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  data_block->asyncaddrinfo_fd = asyncaddrinfo_resolve(hostname, port, &hints);

  if (poll_wait_for_readability(
          p,
          data_block->asyncaddrinfo_fd,
          data_block,
          true,
          false,
          (poll_callback)handle_asyncaddrinfo_resolve_readability) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add asyncaddrinfo_fd for (%s) -> (%s) into epoll: %s",
        data_block->conn->client_hostport,
        data_block->conn->target_hostport,
        error_desc);
    free(error_desc);
    close(data_block->asyncaddrinfo_fd);
    return -1;
  }

  return 0;
}

void start_connecting_to_target(struct poll* p, struct tunnel_conn* conn) {
  struct connecting_data_block* data_block = malloc(sizeof(struct connecting_data_block));
  data_block->conn = conn;

  // Check blacklist
  // TODO: move blacklist handling to server state
  // To handle large blacklists, we should use a specialised string matching algorithm e.g. Aho-Corasick
  char** blacklist = conn->blacklist;
  int blacklist_len = conn->blacklist_len;
  for (int i = 0; i < blacklist_len; i++) {
    if (strstr(conn->target_host, blacklist[i]) != NULL) {
      conn->is_blocked = true;
      LOG("block target: '%s' as it matches '%s'", data_block->conn->target_host, blacklist[i]);
      reject_client_request(p, data_block->conn);
      free(data_block);
      return;
    }
  }

  if (submit_hostname_lookup(p, data_block, conn->target_host, conn->target_port) < 0) {
    reject_client_request(p, conn);
    free(data_block);
    return;
  }
}
