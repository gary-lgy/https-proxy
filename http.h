#ifndef HTTPS_PROXY_HTTP_H
#define HTTPS_PROXY_HTTP_H

#include "connection.h"

int parse_http_connect_message(char* message, struct connection_t* connection);
int send_successful_connect_response(const struct connection_t* conn);
int send_unsuccessful_connect_response(const struct connection_t* conn);

#endif  // HTTPS_PROXY_HTTP_H
