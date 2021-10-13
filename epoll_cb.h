#ifndef HTTPS_PROXY_EPOLL_CB_H
#define HTTPS_PROXY_EPOLL_CB_H

#include "tunnel_conn.h"
enum epoll_cb_type {
  cb_type_accepted,
  cb_type_connecting,
  cb_type_tunneling,
};

// Discriminated union of all cb structs
struct epoll_cb {
  enum epoll_cb_type type;
};

struct epoll_accepted_cb {
  enum epoll_cb_type type;
  struct tunnel_conn* conn;
};

struct epoll_connecting_cb {
  enum epoll_cb_type type;
  struct tunnel_conn* conn;
  struct addrinfo* host_addrs;
  struct addrinfo* next_addr;
  int sock;
};

// Represents a link between source and destination.
// We switch between two states:
// - reading from source
// - writing to destination
struct epoll_tunneling_cb {
  enum epoll_cb_type type;
  struct tunnel_conn* conn;
  struct tunnel_buffer* buf;
  char* source_hostport;
  char* dest_hostport;
  int polled_fd;
  int opposite_fd;
};

#endif  // HTTPS_PROXY_EPOLL_CB_H
