#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "error.h"

#define CONNECT_BACKLOG 512
#define READ_BUFFER_SIZE 8192

inline __attribute__((always_inline)) void check_err(int status, const char* message) {
  if (status < 0) {
    perror(message);
    exit(EXIT_FAILURE);
  }
}

int parse_port_number(const char* raw) {
  char* endptr;
  int port = (int)strtol(raw, &endptr, 10);
  if (*endptr != '\0') {
    fprintf(stderr, "invalid port number: %s\n", raw);
    exit(EXIT_FAILURE);
  }

  return port;
}

int tcp_listen(int port) {
  int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  check_err(listen_socket, "failed to create listen socket");

  struct sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(port);

  check_err(bind(listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)), "failed to bind to port");

  check_err(listen(listen_socket, CONNECT_BACKLOG), "failed to listen");

  return listen_socket;
}

int accept_client_socket(int listen_socket) {
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);
  int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &addr_len);
  check_err(client_socket, "failed to accept incoming connection");

  printf("Received connection from %s:%hu\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

  return client_socket;
}

int tcp_connect(struct error** err, const struct sockaddr* remote_addr, socklen_t remote_addr_len) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    if (err != NULL) {
      *err = new_error_from_errno("failed to create TCP socket");
    }
    return -1;
  }

#ifndef NDEBUG
  struct sockaddr_in* addr = (struct sockaddr_in*)remote_addr;
  fprintf(stderr, "tcp connecting to %s:%hu\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
#endif

  if (connect(sock, remote_addr, remote_addr_len) < 0) {
    close(sock);
    if (err != NULL) {
      *err = new_error_from_errno("failed to create TCP socket");
    }
    return -1;
  }

  return sock;
}

void lookup_host_addr(struct error** err, const char* hostname, struct addrinfo** results) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  // TODO: what to do with port number?
  int gai_errno = getaddrinfo(hostname, "http", &hints, results);
  if (gai_errno != 0 && err != NULL) {
    *err = new_error(append_error_messages("host resolution failed", gai_strerror(gai_errno)), NULL);
  }
}

int tcp_connect_using_hostname(struct error** err, const char* hostname) {
  struct addrinfo *host_addrs, *rp;

  lookup_host_addr(err, hostname, &host_addrs);
  if (err != NULL && *err != NULL) {
    return -1;
  }

  int sock;
  for (rp = host_addrs; rp != NULL; rp = rp->ai_next) {
    if ((sock = tcp_connect(NULL, rp->ai_addr, rp->ai_addrlen)) > 0) {
#ifndef NDEBUG
      struct sockaddr_in* addr = (struct sockaddr_in*)rp->ai_addr;
      printf("connected to server: %s:%hu\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
#endif
      // succeeded
      break;
    }
  }
  freeaddrinfo(host_addrs);

  if (rp == NULL) {  // No address succeeded
    if (err != NULL) {
      *err = new_error("failed to connect to server", NULL);
    }
    return -1;
  }

  return sock;
}

void http_get(int sock, const char* path) {
  char* request_line;
  int request_size = asprintf(&request_line, "GET %s HTTP/1.0\r\n\r\n", path);

#ifndef NDEBUG
  fprintf(stderr, "request: %s\n", request_line);
#endif

  write(sock, request_line, request_size);
}

void relay_http_response(int client_socket, int server_socket) {
  char buffer[READ_BUFFER_SIZE];
  int bytes_read;
  while ((bytes_read = read(server_socket, buffer, READ_BUFFER_SIZE - 1)) > 0) {
    write(client_socket, buffer, bytes_read);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "provide port number as the first argument\n");
    exit(EXIT_FAILURE);
  }

  int listen_port = parse_port_number(argv[1]);
  int listen_socket = tcp_listen(listen_port);
  printf("Listening on port %d\n", listen_port);

  int client_socket = accept_client_socket(listen_socket);

  char read_buffer[READ_BUFFER_SIZE];
  int n_bytes_read = read(client_socket, read_buffer, READ_BUFFER_SIZE - 1);
  check_err(n_bytes_read, "failed to read from client socket");
  read_buffer[n_bytes_read] = '\0';

  // Trim whitespace from hostname
  char* hostname = read_buffer + n_bytes_read - 1;
  while (isspace(*hostname)) {
    hostname--;
  }
  *(hostname + 1) = '\0';
  hostname = read_buffer;
  while (*hostname == '\0') {
    hostname++;
  }

  struct error* err;
  int server_socket = tcp_connect_using_hostname(&err, read_buffer);
  if (err != NULL) {
    fprintf(stderr, "%s", format_error_messages(err));
    exit(EXIT_FAILURE);
  }

  http_get(server_socket, hostname);
  relay_http_response(client_socket, server_socket);

  check_err(close(client_socket), "failed to close client socket");
  check_err(close(server_socket), "failed to close server socket");

  check_err(close(listen_socket), "failed to close listen socket");

  return 0;
}