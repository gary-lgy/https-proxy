#ifndef HTTPS_PROXY_HTTP_H
#define HTTPS_PROXY_HTTP_H

int parse_http_connect_message(char* message, char** host_parsed, char** port_parsed, char** http_version_parsed);

#endif  // HTTPS_PROXY_HTTP_H
