#ifndef HTTPS_PROXY_EPOLL_CB_H
#define HTTPS_PROXY_EPOLL_CB_H

enum epoll_cb_type {
  cb_type_accepted,
  cb_type_connecting,
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

#endif  // HTTPS_PROXY_EPOLL_CB_H
