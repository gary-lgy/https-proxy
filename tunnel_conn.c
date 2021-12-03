#include "tunnel_conn.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct tunnel_conn* create_tunnel_conn(bool telemetry_enabled, char** blacklist, int blacklist_len) {
  struct tunnel_conn* conn = calloc(1, sizeof(struct tunnel_conn));

  conn->client_socket = -1;
  conn->client_socket_dup = -1;
  conn->target_socket = -1;
  conn->target_socket_dup = -1;

  conn->target_host = calloc(MAX_HOST_LEN, sizeof(char));
  conn->target_port = calloc(MAX_PORT_LEN, sizeof(char));
  conn->http_version = calloc(HTTP_VERSION_LEN, sizeof(char));
  conn->client_hostport = calloc(HOST_PORT_BUF_SIZE, sizeof(char));
  conn->target_hostport = calloc(HOST_PORT_BUF_SIZE, sizeof(char));

  char* buffer = malloc(BUFFER_SIZE * sizeof(char));
  conn->to_target_buffer.start = buffer;
  conn->to_target_buffer.read_ptr = buffer;
  conn->to_target_buffer.write_ptr = buffer;

  buffer = malloc(BUFFER_SIZE * sizeof(char));
  conn->to_client_buffer.start = buffer;
  conn->to_client_buffer.read_ptr = buffer;
  conn->to_client_buffer.write_ptr = buffer;

  conn->halves_closed = 0;
  conn->n_bytes_transferred = 0;

  conn->telemetry_enabled = telemetry_enabled;
  if (telemetry_enabled) {
    timespec_get(&conn->started_at, TIME_UTC);
  }

  conn->blacklist = blacklist;
  conn->blacklist_len = blacklist_len;
  conn->is_blocked = false;

  return conn;
}

void destroy_tunnel_conn(struct tunnel_conn* conn) {
  if (conn->telemetry_enabled && conn->target_host[0] != '\0' && !conn->is_blocked) {
    struct timespec ended_at;
    timespec_get(&ended_at, TIME_UTC);
    unsigned int milliseconds_elapsed =
        (ended_at.tv_sec - conn->started_at.tv_sec) * 1000 + (ended_at.tv_nsec - conn->started_at.tv_nsec) / 1000000;
    printf(
        "Hostname: %s, Size: %llu bytes, Time: %.3f sec\n",
        conn->target_host,
        conn->n_bytes_transferred,
        milliseconds_elapsed / 1000.0);
  }

  if (conn->client_socket_dup >= 0) {
    close(conn->client_socket_dup);
  }

  if (conn->client_socket >= 0) {
    shutdown(conn->client_socket, SHUT_RDWR);
    close(conn->client_socket);
  }

  if (conn->target_socket_dup >= 0) {
    close(conn->target_socket_dup);
  }

  if (conn->target_socket >= 0) {
    shutdown(conn->target_socket, SHUT_RDWR);
    close(conn->target_socket);
  }

  free(conn->client_hostport);
  free(conn->target_hostport);
  free(conn->target_host);
  free(conn->target_port);
  free(conn->http_version);
  free(conn->to_target_buffer.start);
  free(conn->to_client_buffer.start);

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
