#ifndef HTTPS_PROXY_EPOLL_CB_H
#define HTTPS_PROXY_EPOLL_CB_H

#include "../tunnel_conn.h"

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
  int asyncaddrinfo_fd;
  struct addrinfo* host_addrs;
  struct addrinfo* next_addr;
  int target_conn_sock;
  bool failed;
};

// Represents a link between source and destination.
// We switch between two states:
// - reading from source
// - writing to destination
struct epoll_tunneling_cb {
  enum epoll_cb_type type;
  struct tunnel_conn* conn;
  bool is_client_to_target;
  bool is_read;
};

void accept_incoming_connections(int epoll_fd, int listening_socket, bool telemetry_enabled);
void handle_accepted_cb(int epoll_fd, struct epoll_accepted_cb* cb, uint32_t events);

void enter_connecting_state(int epoll_fd, struct tunnel_conn* conn);
void handle_connecting_cb(int epoll_fd, struct epoll_connecting_cb* cb, uint32_t events);

void handle_tunneling_cb(int epoll_fd, struct epoll_tunneling_cb* cb, uint32_t events);
void enter_tunneling_state(int epoll_fd, struct tunnel_conn* conn);

#endif  // HTTPS_PROXY_EPOLL_CB_H
