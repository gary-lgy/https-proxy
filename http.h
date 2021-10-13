#ifndef HTTPS_PROXY_HTTP_H
#define HTTPS_PROXY_HTTP_H

#include "tunnel_conn.h"

int parse_http_connect_message(char* message, char** host_parsed, char** port_parsed, char** http_version_parsed);
int send_successful_connect_response(const struct tunnel_conn* conn);
int send_unsuccessful_connect_response(const struct tunnel_conn* conn);

#endif  // HTTPS_PROXY_HTTP_H
