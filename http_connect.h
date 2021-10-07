#ifndef HTTPS_PROXY_HTTP_CONNECT_H
#define HTTPS_PROXY_HTTP_CONNECT_H

int parse_http_connect_message(char* message, const char** host, const char** port);

#endif  // HTTPS_PROXY_HTTP_CONNECT_H
