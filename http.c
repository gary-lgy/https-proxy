#include "http.h"
#include <string.h>

#define DEFAULT_TARGET_PORT "443"

int parse_http_connect_message(char* message, char** host_parsed, char** port_parsed, char** http_version_parsed) {
  // CONNECT google.com:443 HTTP/1.0
  char* saveptr;
  char* connect_token = strtok_r(message, " ", &saveptr);
  if (connect_token == NULL || strcmp(connect_token, "CONNECT") != 0) {
    return -1;
  }

  char* host_port_token = strtok_r(NULL, " ", &saveptr);
  if (host_port_token == NULL) {
    return -1;
  }
  char* host_port_saveptr;
  char* host = strtok_r(host_port_token, ":", &host_port_saveptr);
  char* port = strtok_r(NULL, ":", &host_port_saveptr);
  if (port == NULL) {
    port = DEFAULT_TARGET_PORT;
  }

  // HTTP/1.1 or HTTP/1.0
  char* http_version = strtok_r(NULL, " \r\n", &saveptr);
  if (http_version == NULL || (strcmp(http_version, "HTTP/1.0") != 0 && strcmp(http_version, "HTTP/1.1") != 0)) {
    return -1;
  }

  *host_parsed = host;
  *port_parsed = port;
  *http_version_parsed = http_version;

  return 0;
}
