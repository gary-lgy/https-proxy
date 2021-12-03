#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../log.h"
#include "../poll.h"
#include "../util.h"
#include "tunnel_conn.h"

// Represents a (uni-directional) link between source and destination.
// The link alternates between two states:
// 1. reading from source
// 2. writing to destination
struct tunneling_link {
  struct tunnel_conn* conn;
  int read_fd;
  int write_fd;
  struct tunnel_buffer* buf;
  const char* source_hostport;
  const char* dst_hostport;
};

void link_wait_to_read(struct poll* p, struct tunneling_link* link);
void link_wait_to_write(struct poll* p, struct tunneling_link* link);
void handle_link_readability(struct poll* p, struct tunneling_link* link);
void handle_link_writability(struct poll* p, struct tunneling_link* link);

void setup_tunneling_from_target_to_client(struct poll* p, struct tunnel_conn* conn) {
  // First, send HTTP 200 to client
  int n_bytes = sprintf(conn->to_client_buffer.start, "%s 200 Connection Established \r\n\r\n", conn->http_version);
  conn->to_client_buffer.write_ptr += n_bytes;

  struct tunneling_link* link = malloc(sizeof(struct tunneling_link));
  link->conn = conn;
  link->read_fd = conn->target_socket;
  link->write_fd = conn->client_socket_dup;
  link->buf = &conn->to_client_buffer;
  link->source_hostport = conn->target_hostport;
  link->dst_hostport = conn->client_hostport;

  link_wait_to_write(p, link);
}

void setup_tunneling_from_client_to_target(struct poll* p, struct tunnel_conn* conn) {
  struct tunneling_link* link = malloc(sizeof(struct tunneling_link));
  link->conn = conn;
  link->read_fd = conn->client_socket;
  link->write_fd = conn->target_socket_dup;
  link->buf = &conn->to_target_buffer;
  link->source_hostport = conn->client_hostport;
  link->dst_hostport = conn->target_hostport;

  size_t n_bytes_remaining = conn->to_target_buffer.write_ptr - conn->to_target_buffer.read_ptr;
  if (n_bytes_remaining > 0) {
    // if we received more than just the CONNECT message from the client, send the rest of the bytes to the target
    DEBUG_LOG("sending %d left over bytes after CONNECT", n_bytes_remaining);

    link_wait_to_write(p, link);
  } else {
    // wait to read from client

    // reset the buffer
    conn->to_target_buffer.read_ptr = conn->to_target_buffer.start;
    conn->to_target_buffer.write_ptr = conn->to_target_buffer.start;

    link_wait_to_read(p, link);
  }
}

void start_tunneling(struct poll* p, struct tunnel_conn* conn) {
  // dup each socket to decouple read and write ends of the socket
  // this allows us to wait for its readability and writability separately
  // use the original fd for reading; use the dupped fd for writing
  conn->client_socket_dup = dup(conn->client_socket);
  conn->target_socket_dup = dup(conn->target_socket);

  // set up a tunneling link for both directions
  setup_tunneling_from_target_to_client(p, conn);
  setup_tunneling_from_client_to_target(p, conn);
}

void link_wait_to_read(struct poll* p, struct tunneling_link* link) {
  if (poll_wait_for_readability(p, link->read_fd, link, true, false, (poll_callback)handle_link_readability) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to wait on read_fd of (%s) -> (%s) for readability: %s",
        link->source_hostport,
        link->dst_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(link->conn);
    free(link);
    return;
  }
}

void link_wait_to_write(struct poll* p, struct tunneling_link* link) {
  if (poll_wait_for_writability(p, link->write_fd, link, true, false, (poll_callback)handle_link_writability) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to wait on write_fd of (%s) -> (%s) for writability: %s",
        link->source_hostport,
        link->dst_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(link->conn);
    free(link);
    return;
  }
}

void handle_link_readability(struct poll* p, struct tunneling_link* link) {
  size_t remaining_capacity = BUFFER_SIZE - (link->buf->write_ptr - link->buf->start);
  if (remaining_capacity <= 0) {
    die(hsprintf(
        "going to read for tunnel (%s) -> (%s), but the buf is full; this should not happen",
        link->source_hostport,
        link->dst_hostport));
  }

  ssize_t n_bytes_read = read(link->read_fd, link->buf->write_ptr, remaining_capacity);

  if (n_bytes_read == 0) {
    // peer stopped sending
    LOG("peer (%s) -> (%s) closed connection", link->source_hostport, link->dst_hostport);
    shutdown(link->read_fd, SHUT_RD);
    shutdown(link->write_fd, SHUT_WR);
    if (++link->conn->halves_closed == 2) {
      LOG("tunnel (%s) -> (%s) closed", link->conn->client_hostport, link->conn->target_hostport);
      // both halves closed, tear down the whole connection
      destroy_tunnel_conn(link->conn);
      free(link);
    }
    return;
  } else if (n_bytes_read < 0) {
    // read error
    char* error_desc = errno2s(errno);
    LOG("read error from (%s) -> (%s): %s", link->source_hostport, link->dst_hostport, error_desc);
    free(error_desc);

    destroy_tunnel_conn(link->conn);
    free(link);
    return;
  }

  DEBUG_LOG("received %zu bytes (%s) -> (%s)", n_bytes_read, link->source_hostport, link->dst_hostport);
  link->buf->write_ptr += n_bytes_read;
  link->conn->n_bytes_transferred += n_bytes_read;

  // we will then write into write_fd
  link_wait_to_write(p, link);
}

void handle_link_writability(struct poll* p, struct tunneling_link* link) {
  size_t n_bytes_to_send = link->buf->write_ptr - link->buf->read_ptr;

  if (n_bytes_to_send <= 0) {
    die(hsprintf(
        "going to write for tunnel (%s) -> (%s), but the buf is empty; this should not happen",
        link->source_hostport,
        link->dst_hostport));
  }

  ssize_t n_bytes_sent = send(link->write_fd, link->buf->read_ptr, n_bytes_to_send, MSG_NOSIGNAL);

  if (n_bytes_sent < 0) {
    // peer refused to receive?
    // teardown the entire connection
    char* error_desc = errno2s(errno);
    LOG("write error from (%s) -> (%s): %s", link->source_hostport, link->dst_hostport, error_desc);
    free(error_desc);

    destroy_tunnel_conn(link->conn);
    free(link);
    return;
  }

  DEBUG_LOG("wrote %zu bytes (%s) -> (%s)", n_bytes_sent, link->source_hostport, link->dst_hostport);

  link->buf->read_ptr += n_bytes_sent;

  if (link->buf->read_ptr >= link->buf->write_ptr) {
    // sent everything, we can read again
    link->buf->read_ptr = link->buf->write_ptr = link->buf->start;

    link_wait_to_read(p, link);
  } else {
    // We didn't manage to send all the bytes.
    // This can happen when the TCP buffer is full for a slow receiver.
    // Wait for writability to send again later.
    link_wait_to_write(p, link);
  }
}
