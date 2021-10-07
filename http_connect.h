#ifndef HTTPS_PROXY_HTTP_CONNECT_H
#define HTTPS_PROXY_HTTP_CONNECT_H

struct http_connect_request {
  const char* host;
  const char* port;
  const char* http_version;
};

int parse_http_connect_message(char* message, struct http_connect_request* result);
int send_successful_connect_response(int sock, const struct http_connect_request* request);

#endif  // HTTPS_PROXY_HTTP_CONNECT_H
