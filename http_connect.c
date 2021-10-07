#include "http_connect.h"
#include <malloc.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_TARGET_PORT "80"

int parse_http_connect_message(char* message, struct http_connect_request* result) {
  // CONNECT google.com:443 HTTP/1.0
  char* saveptr;
  char* token = strtok_r(message, " ", &saveptr);
  if (token == NULL || strcmp(token, "CONNECT") != 0) {
    return -1;
  }

  char* host_port_token = strtok_r(NULL, " ", &saveptr);
  char* host_port_saveptr;
  const char* host = strtok_r(host_port_token, ":", &host_port_saveptr);
  const char* port = strtok_r(NULL, ":", &host_port_saveptr);
  if (port == NULL) {
    port = DEFAULT_TARGET_PORT;
  }

  // HTTP/1.1 or HTTP/1.0
  const char* http_version = strtok_r(NULL, " \r\n", &saveptr);
  if (http_version == NULL || (strcmp(http_version, "HTTP/1.0") != 0 && strcmp(http_version, "HTTP/1.1") != 0)) {
    return -1;
  }

  result->host = strdup(host);
  result->port = strdup(port);
  result->http_version = strdup(http_version);

  return 0;
}

int send_successful_connect_response(int sock, const struct http_connect_request* request) {
  char* response_line;
  int response_size = asprintf(&response_line, "%s 200 Connection established \r\n\r\n", request->http_version);

  write(sock, response_line, response_size);
  free(response_line);
  return 0;
}