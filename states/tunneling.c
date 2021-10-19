#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../http.h"
#include "../log.h"
#include "../tunnel_conn.h"
#include "../util.h"
#include "epoll_cb.h"

void enter_tunneling_state(int epoll_fd, struct tunnel_conn* conn) {
  // dup each socket to decouple read and write ends of the socket
  // this allows us to set different event masks on the same socket
  int client_read_fd = conn->client_socket;
  int client_write_fd = conn->client_socket_dup = dup(client_read_fd);
  int target_read_fd = conn->target_socket;
  int target_write_fd = conn->target_socket_dup = dup(target_read_fd);

  // Send HTTP 200 to client
  int n_bytes =
      sprintf(conn->target_to_client_buffer.start, "%s 200 Connection Established \r\n\r\n", conn->http_version);
  conn->target_to_client_buffer.write_ptr += n_bytes;

  struct epoll_tunneling_cb* cb = malloc(sizeof(struct epoll_tunneling_cb));
  cb->type = cb_type_tunneling;
  cb->conn = conn;
  cb->is_client_to_target = false;  // destination is client
  cb->is_read = false;              // writing to client

  struct epoll_event event;
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

  // During tunneling, we will switch to epoll_wait on all these FDs.
  // If the FD has already been added, EPOLL_CTL_ADD will not work; only EPOLL_CTL_MOD can reactivate the FD.
  // On the other hand, EPOLL_CTL_MOD will only work for FDs that have not been added.
  // However, we have no idea which FDs are in the epoll instance already.
  // For convenience, we make sure all FDs have been added to the epoll instance.
  // Hence, we can use EPOLL_CTL_MOD later when we want to wait on the socket again.

  // Add target_write_fd to epoll since it is never added before.
  // The other 3 FDs have already been added.
  event.events = EPOLLERR | EPOLLONESHOT;
  event.data.ptr = NULL;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, target_write_fd, &event) < 0) {
    char* error_desc = errno2s(errno);
    DEBUG_LOG(
        "failed to add target_write_fd %d for (%s) -> (%s) into epoll: %s",
        target_write_fd,
        conn->client_hostport,
        conn->target_hostport,
        error_desc);
    free(error_desc);

    destroy_tunnel_conn(conn);
    free(cb);
    return;
  }

  size_t n_bytes_remaining = conn->client_to_target_buffer.write_ptr - conn->client_to_target_buffer.read_ptr;
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
    conn->client_to_target_buffer.read_ptr = conn->client_to_target_buffer.start;
    conn->client_to_target_buffer.write_ptr = conn->client_to_target_buffer.start;

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

void handle_tunneling_read(
    int epoll_fd,
    struct epoll_tunneling_cb* cb,
    const char* source_hostport,
    const char* dest_hostport,
    struct tunnel_buffer* buf,
    int polled_fd,
    int opposite_fd) {
  size_t remaining_capacity = BUFFER_SIZE - (buf->write_ptr - buf->start);
  if (remaining_capacity <= 0) {
    die(hsprintf(
        "going to read for tunnel (%s) -> (%s), but the buf is full; this should not happen",
        source_hostport,
        dest_hostport));
  }

  ssize_t n_bytes_read = read(polled_fd, buf->write_ptr, remaining_capacity);

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

  buf->write_ptr += n_bytes_read;

  // we will then write into opposite_fd
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
  size_t n_bytes_to_send = buf->write_ptr - buf->read_ptr;

  if (n_bytes_to_send <= 0) {
    die(hsprintf(
        "going to write for tunnel (%s) -> (%s), but the buf is empty; this should not happen",
        source_hostport,
        dest_hostport));
  }

  ssize_t n_bytes_sent = send(polled_fd, buf->read_ptr, n_bytes_to_send, MSG_NOSIGNAL);

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

  buf->read_ptr += n_bytes_sent;

  struct epoll_event event;
  if (buf->read_ptr >= buf->write_ptr) {
    // sent everything, we can read again
    buf->read_ptr = buf->write_ptr = buf->start;

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
    // We didn't manage to send all the bytes.
    // This can happen when the TCP buffer is full for a slow receiver.
    // Wait for writability to send again later.
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
