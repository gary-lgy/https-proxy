#ifndef HTTPS_PROXY_PROXY_SERVER_H
#define HTTPS_PROXY_PROXY_SERVER_H

#include "tunnel_conn.h"

struct proxy_server {
  int listening_socket;
  bool telemetry_enabled;
  char** blacklist;
  int blacklist_len;
};

void accept_incoming_connections(struct poll* p, struct proxy_server* server);

void start_connecting_to_target(struct poll* p, struct tunnel_conn* conn);

void start_tunneling(struct poll* p, struct tunnel_conn* conn);

#endif  // HTTPS_PROXY_PROXY_SERVER_H
