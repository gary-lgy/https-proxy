#ifndef HTTPS_PROXY_HTTP_H
#define HTTPS_PROXY_HTTP_H

#include "tunnel_conn.h"

int parse_http_connect_message(char* message, struct tunnel_conn* connection);
int send_successful_connect_response(const struct tunnel_conn* conn);
int send_unsuccessful_connect_response(const struct tunnel_conn* conn);

#endif  // HTTPS_PROXY_HTTP_H
