#include "tunnel_conn.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tunnel_conn* init_conn() {
  struct tunnel_conn* conn = calloc(1, sizeof(struct tunnel_conn));

  conn->client_socket = -1;
  conn->target_socket = -1;
  conn->target_host = calloc(MAX_HOST_LEN, sizeof(char));
  conn->target_port = calloc(MAX_PORT_LEN, sizeof(char));
  conn->http_version = calloc(HTTP_VERSION_LEN, sizeof(char));
  conn->client_hostport = calloc(HOST_PORT_BUF_SIZE, sizeof(char));
  conn->target_hostport = calloc(HOST_PORT_BUF_SIZE, sizeof(char));

  char* buffer = malloc(BUFFER_SIZE * sizeof(char));
  conn->client_to_target_buffer.start = buffer;
  conn->client_to_target_buffer.consumable = buffer;
  conn->client_to_target_buffer.empty = buffer;

  buffer = malloc(BUFFER_SIZE * sizeof(char));
  conn->target_to_client_buffer.start = buffer;
  conn->target_to_client_buffer.consumable = buffer;
  conn->target_to_client_buffer.empty = buffer;

  return conn;
}

void free_conn(struct tunnel_conn* conn) {
  if (conn->client_socket > 0) {
    shutdown(conn->client_socket, SHUT_RDWR);
    close(conn->client_socket);
  }

  if (conn->target_socket > 0) {
    shutdown(conn->target_socket, SHUT_RDWR);
    close(conn->target_socket);
  }

  free(conn->client_hostport);
  free(conn->target_hostport);
  free(conn->target_host);
  free(conn->target_port);
  free(conn->http_version);
  free(conn->client_to_target_buffer.start);
  free(conn->target_to_client_buffer.start);

  free(conn);
}

void set_client_hostport(struct tunnel_conn* conn, const struct sockaddr_in* client_addr) {
  inet_ntop(AF_INET, &client_addr->sin_addr, conn->client_hostport, INET_ADDRSTRLEN);
  strcat(conn->client_hostport, ":");
  char client_port[MAX_PORT_LEN];
  sprintf(client_port, "%hu", ntohs(client_addr->sin_port));
  strcat(conn->client_hostport, client_port);
}

void set_target_hostport(struct tunnel_conn* conn) {
  strcpy(conn->target_hostport, conn->target_host);
  strcat(conn->target_hostport, ":");
  strcat(conn->target_hostport, conn->target_port);
}
