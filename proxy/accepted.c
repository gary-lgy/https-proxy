#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../log.h"
#include "../poll.h"
#include "../util.h"
#include "proxy_server.h"

#define DEFAULT_TARGET_PORT "443"

void handle_client_connect_request_readability(struct poll* p, struct tunnel_conn* conn);

void accept_incoming_connections(struct poll* p, struct proxy_server* server) {
  // accept all pending connections
  while (1) {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(struct sockaddr_in);

    int client_socket = accept4(server->listening_socket, (struct sockaddr*)&client_addr, &addrlen, SOCK_NONBLOCK);
    if (client_socket < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // finished processing all incoming connections
        return;
      } else {
        // unexpected error in accepting the connection
        char* error_desc = errno2s(errno);
        DEBUG_LOG("accept failed: %s", error_desc);
        free(error_desc);
        return;
      }
    }

    struct tunnel_conn* conn = create_tunnel_conn(server->telemetry_enabled, server->blocklist, server->blocklist_len);
    conn->client_socket = client_socket;
    set_client_hostport(conn, &client_addr);

    LOG("Received connection from %s", conn->client_hostport);

    // wait for client socket readability so we can read its CONNECT HTTP request
    if (poll_wait_for_readability(
            p, client_socket, conn, true, false, (poll_callback)handle_client_connect_request_readability) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG(
          "failed to add accepted client socket from %s into poll instance: %s", conn->client_hostport, error_desc);
      free(error_desc);

      destroy_tunnel_conn(conn);
    }
  }
}

/**
 * @param read_fd
 * @param buf
 * @return the number of bytes read on success;
 * -1 when reading error is encountered;
 * -2 if the buffer is full.
 */
ssize_t read_into_buffer(int read_fd, struct tunnel_buffer* buf) {
  // We always want the contents in the buffer to be null terminated, even if no data is read
  buf->write_ptr[0] = '\0';

  // Leave one byte for null terminator
  size_t remaining_capacity = BUFFER_SIZE - 1 - (buf->write_ptr - buf->start);
  if (remaining_capacity <= 0) {
    return -2;
  }

  ssize_t n_bytes_read = read(read_fd, buf->write_ptr, remaining_capacity);

  if (n_bytes_read <= 0) {
    return n_bytes_read;
  }

  buf->write_ptr += n_bytes_read;
  buf->write_ptr[0] = '\0';
  return n_bytes_read;
}

int parse_http_connect_message(char* message, char** host_parsed, char** port_parsed, char** http_version_parsed) {
  // CONNECT google.com:443 HTTP/1.0
  char* saveptr;
  char* connect_token = strtok_r(message, " ", &saveptr);
  if (connect_token == NULL || strcmp(connect_token, "CONNECT") != 0) {
    return -1;
  }

  char* host_port_token = strtok_r(NULL, " ", &saveptr);
  if (host_port_token == NULL) {
    return -1;
  }
  char* host_port_saveptr;
  char* host = strtok_r(host_port_token, ":", &host_port_saveptr);
  char* port = strtok_r(NULL, ":", &host_port_saveptr);
  if (port == NULL) {
    port = DEFAULT_TARGET_PORT;
  }

  // HTTP/1.1 or HTTP/1.0
  char* http_version = strtok_r(NULL, " \r\n", &saveptr);
  if (http_version == NULL || (strcmp(http_version, "HTTP/1.0") != 0 && strcmp(http_version, "HTTP/1.1") != 0)) {
    return -1;
  }

  *host_parsed = host;
  *port_parsed = port;
  *http_version_parsed = http_version;

  return 0;
}

/**
 * @param conn
 * @return -1 if an error occurred and conn should be closed;
 * 0 if CONNECT was found and parsed;
 * 1 if we need to read more bytes.
 */
int read_connect_request(struct tunnel_conn* conn) {
  struct tunnel_buffer* buf = &conn->to_target_buffer;
  ssize_t n_bytes_read = read_into_buffer(conn->client_socket, buf);

  if (n_bytes_read < 0) {
    char* errno_desc = errno2s(errno);
    LOG("reading for CONNECT from %s failed: %s, received %d bytes",
        conn->client_hostport,
        errno_desc,
        buf->write_ptr - buf->start);
    free(errno_desc);
    return -1;
  }

  if (n_bytes_read == 0) {
    LOG("client %s closed the connection before sending full http CONNECT message, received %d bytes: %s",
        conn->client_hostport,
        buf->write_ptr - buf->start,
        buf->start);
    return -1;
  }

  char* double_crlf = strstr(buf->start, "\r\n\r\n");
  if (double_crlf != NULL) {
    // received full CONNECT message
    char *host, *port, *http_version;
    if (parse_http_connect_message(buf->start, &host, &port, &http_version) < 0) {
      // malformed CONNECT
      LOG("couldn't parse CONNECT message: %s", buf->start);
      return -1;
    }

    strncpy(conn->target_host, host, MAX_HOST_LEN);
    strncpy(conn->target_port, port, MAX_PORT_LEN);
    strncpy(conn->http_version, http_version, HTTP_VERSION_LEN);

    set_target_hostport(conn);

    buf->read_ptr = double_crlf + 4;  // skip over the double crlf

    LOG("received CONNECT request: %s %s:%s", conn->http_version, conn->target_host, conn->target_port);

    return 0;
  }

  // we don't have an HTTP message yet, can we read more bytes?

  if (buf->write_ptr >= buf->start + BUFFER_SIZE - 1) {
    // no, the buffer is full
    LOG("no CONNECT message from %s until buffer is full", conn->client_hostport);
    return -1;
  }

  // let's read more bytes
  return 1;
}

void handle_client_connect_request_readability(struct poll* p, struct tunnel_conn* conn) {
  int result = read_connect_request(conn);
  if (result < 0) {
    destroy_tunnel_conn(conn);
  } else if (result == 0) {
    // we have the full CONNECT message, let's connect to the target
    start_connecting_to_target(p, conn);
  } else {
    // need to read more bytes, wait for readability again
    if (poll_wait_for_readability(
            p, conn->client_socket, conn, true, false, (poll_callback)handle_client_connect_request_readability) < 0) {
      char* error_desc = errno2s(errno);
      DEBUG_LOG("failed to re-add client socket from %s for reading CONNECT: %s", conn->client_hostport, error_desc);
      free(error_desc);

      destroy_tunnel_conn(conn);
    }
  }
}
