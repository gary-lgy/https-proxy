#ifndef HTTPS_PROXY_TUNNEL_CONN_H
#define HTTPS_PROXY_TUNNEL_CONN_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdlib.h>

#define BUFFER_SIZE (1024 * 8)

#define MAX_HOST_LEN 512
#define MAX_PORT_LEN 6
#define HTTP_VERSION_LEN 9  // HTTP/1.1
#define HOST_PORT_BUF_SIZE 1024

/**
 * The buffer layout looks like this:
 * The first section is data that has already been written and read.
 * The second section is data that is written but yet to be read from the buffer.
 * The third section is available space for writing into the buffer.
 * |-------------------| <--- start
 * |     used data     |
 * |-------------------| <--- read_ptr
 * |    can be read    |
 * |-------------------| <--- write_ptr
 * |   can be written  |
 * |-------------------|
 *
 * When writing into the buffer, producers should write from the start of `write_ptr`.
 * When reading from the buffer, consumers should read from the start if `read_ptr`.
 * The boundaries should be adjusted accordingly after reading / writing.
 */
struct tunnel_buffer {
  char* start;
  char* read_ptr;
  char* write_ptr;
};

struct tunnel_conn {
  // file descriptors
  int client_socket;
  int client_socket_dup;
  int target_socket;
  int target_socket_dup;

  // textual representations of ip/hostname:port for printing
  char* client_hostport;
  char* target_hostport;

  // obtained from the CONNECT HTTP message
  char* target_host;
  char* target_port;
  char* http_version;

  // buffers for tunneling
  struct tunnel_buffer client_to_target_buffer;
  struct tunnel_buffer target_to_client_buffer;

  // how many directions of this connection have been closed (0, 1, or 2)
  int halves_closed;

  // telemetry
  bool telemetry_enabled;
  struct timespec started_at;
  unsigned long long n_bytes_streamed;
};

struct tunnel_conn* create_tunnel_conn(bool telemetry_enabled);
void destroy_tunnel_conn(struct tunnel_conn* conn);
void set_client_hostport(struct tunnel_conn*, const struct sockaddr_in*);
void set_target_hostport(struct tunnel_conn*);

#endif  // HTTPS_PROXY_TUNNEL_CONN_H
