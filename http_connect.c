#include <string.h>

#define DEFAULT_HTTPS_PORT "80"

int parse_http_connect_message(char* message, const char** host, const char** port) {
  // CONNECT google.com:443 HTTP/1.0
  char* saveptr;
  char* token = strtok_r(message, " ", &saveptr);
  if (token == NULL || strcmp(token, "CONNECT") != 0) {
    return -1;
  }

  char* hostname_token = strtok_r(NULL, " ", &saveptr);
  char* host_port_saveptr;
  *host = strdup(strtok_r(hostname_token, ":", &host_port_saveptr));
  *port = strtok_r(NULL, ":", &host_port_saveptr);
  if (*port == NULL) {
    // port not specified
    *port = DEFAULT_HTTPS_PORT;
  }

  // HTTP/1.1 or HTTP/1.0
  token = strtok_r(NULL, " ", &saveptr);
  if (token == NULL || (strcmp(token, "HTTP/1.0") != 0 || strcmp(token, "HTTP/1.1") != 0)) {
    return -1;
  }

  return 0;
}
