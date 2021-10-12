#ifndef HTTPS_PROXY_CONNECTION_H
#define HTTPS_PROXY_CONNECTION_H

#include <netinet/in.h>
#include <stdlib.h>

#define BUFFER_SIZE 8192

#define MAX_HOST_LEN 512
#define MAX_PORT_LEN 6
#define HTTP_VERSION_LEN 9  // HTTP/1.1
#define HOST_PORT_BUF_SIZE 1024

struct connection_t {
  // socket addresses
  struct sockaddr_in* client_addr;
  struct sockaddr_in* target_addr;

  // file descriptors
  int client_socket;
  int target_socket;

  // textual representations of the ip/hostname:port for printing
  char* client_hostport;
  char* target_hostport;

  // obtained from the CONNECT HTTP message
  char* target_host;
  char* target_port;
  char* http_version;

  // buffers for tunneling
  char* client_to_target_buffer;
  char* target_to_client_buffer;
};

struct connection_t* init_conn();
void free_conn(struct connection_t*);
void set_client_hostport(struct connection_t*);
void set_target_hostport(struct connection_t*);

#endif  // HTTPS_PROXY_CONNECTION_H
