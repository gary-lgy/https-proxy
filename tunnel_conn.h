#ifndef HTTPS_PROXY_TUNNEL_CONN_H
#define HTTPS_PROXY_TUNNEL_CONN_H

#include <netinet/in.h>
#include <stdlib.h>

#define BUFFER_SIZE 8192

#define MAX_HOST_LEN 512
#define MAX_PORT_LEN 6
#define HTTP_VERSION_LEN 9  // HTTP/1.1
#define HOST_PORT_BUF_SIZE 1024

/**
 * The buffer layout looks like this:
 * The first section is data that has already been consumed.
 * The second section is data that is yet to be consumed.
 * The third section is available space.
 * |-------------------| <--- start
 * | already consumed  |
 * |-------------------| <--- consumable
 * |  can be consumed  |
 * |-------------------| <--- empty
 * |      empty        |
 * |-------------------|
 *
 * When writing into the buffer, producers should write from the start of the empty section.
 * When consuming from the buffer, consumers should consume from the second section.
 * The boundaries should be adjusted accordingly after consumption / production.
 */
struct tunnel_buffer {
  char* start;
  char* consumable;
  char* empty;
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
};

struct tunnel_conn* init_conn();
void free_conn(struct tunnel_conn*);
void set_client_hostport(struct tunnel_conn*, const struct sockaddr_in*);
void set_target_hostport(struct tunnel_conn*);

#endif  // HTTPS_PROXY_TUNNEL_CONN_H
