#include "http.h"
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include "connection.h"
#include "util.h"

#define DEFAULT_TARGET_PORT "80"

int parse_http_connect_message(char* message, struct connection_t* connection) {
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

  strncpy(connection->target_host, host, MAX_HOST_LEN);
  strncpy(connection->target_port, port, MAX_PORT_LEN);
  strncpy(connection->http_version, http_version, HTTP_VERSION_LEN);

  strcpy(connection->target_hostport, connection->target_host);
  strcat(connection->target_hostport, ":");
  strcat(connection->target_hostport, connection->target_port);

  return 0;
}

int send_successful_connect_response(const struct connection_t* conn) {
  char* response_line = hsprintf("%s 200 Connection Established \r\n\r\n", conn->http_version);
  ssize_t status = write(conn->client_socket, response_line, strlen(response_line));
  free(response_line);
  return status < 0 ? -1 : 0;
}

int send_unsuccessful_connect_response(const struct connection_t* conn) {
  char* response_line = hsprintf("%s 400 Bad Request \r\n\r\n", conn->http_version);
  ssize_t status = write(conn->client_socket, response_line, strlen(response_line));
  free(response_line);
  return status < 0 ? -1 : 0;
}
