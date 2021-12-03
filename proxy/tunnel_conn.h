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
 * Producers will write bytes into the buffer,
 * and consumers will read data from the buffer.
 *
 * We will have one buffer for each direction of the tunneling connection.
 * Take the direction from client to target.
 * The proxy will receive data from the client, place it into a buffer, and then send
 * the data to the target.
 * The producer in this direction is the client and the consumer is the target.
 *
 * In the opposite direction where we receive data from the target and send it to the client,
 * the producer is the target and the consumer is the client.
 *
 *
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

/**
 * Represents a tunneling connection.
 * There are two directions to this connection: client to target and target to client.
 * Each direction has its own buffer and sets of socket file descriptors.
 */
struct tunnel_conn {
  // file descriptors
  // Before we start tunneling, only client_socket and target_socket are used
  // After we start tunneling, client_socket_dup is dup(client_socket) and target_socket_dup is dup(target_socket)
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

  /**
   * Buffer for data to be sent to the target.
   */
  struct tunnel_buffer to_target_buffer;
  /**
   * Buffer for data to be sent to the client.
   */
  struct tunnel_buffer to_client_buffer;

  // how many directions of this connection have been closed (0, 1, or 2)
  int halves_closed;

  // telemetry
  bool telemetry_enabled;
  struct timespec started_at;
  unsigned long long n_bytes_transferred;

  // blacklist
  char** blacklist;
  int blacklist_len;
  bool is_blocked;
};

struct tunnel_conn* create_tunnel_conn(bool telemetry_enabled, char** blacklist, int blacklist_len);
void destroy_tunnel_conn(struct tunnel_conn* conn);
void set_client_hostport(struct tunnel_conn*, const struct sockaddr_in*);
void set_target_hostport(struct tunnel_conn*);

#endif  // HTTPS_PROXY_TUNNEL_CONN_H
