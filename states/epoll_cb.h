#ifndef HTTPS_PROXY_EPOLL_CB_H
#define HTTPS_PROXY_EPOLL_CB_H

#include "../proxy_server.h"
#include "../tunnel_conn.h"

// TODO: clean up this file
// TODO: put this into proxy_server.h and rename this directory
// a lot can be removed

void accept_incoming_connections(struct poll* p, struct proxy_server* server);

void start_connecting_to_target(struct poll* p, struct tunnel_conn* conn);

void start_tunneling(struct poll* p, struct tunnel_conn* conn);

#endif  // HTTPS_PROXY_EPOLL_CB_H
