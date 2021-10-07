#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "error.h"
#include "log.h"

#define CONNECT_BACKLOG 512
#define READ_BUFFER_SIZE 8192

inline __attribute__((always_inline)) void die(const char* message) {
  fprintf(stderr, "%s\n", message);
  exit(EXIT_FAILURE);
}

int parse_port_number(struct error** err, const char* raw) {
  char* endptr;
  int port = (int)strtol(raw, &endptr, 10);
  if (*endptr != '\0') {
    if (err != NULL) {
      char* msg;
      asprintf(&msg, "invalid port number: %s\n", raw);
      *err = new_error(msg, NULL);
    }
    return -1;
  }

  return port;
}

int tcp_listen(struct error** err, int port) {
  int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket < 0) {
    if (err != NULL) {
      *err = new_error_from_errno("failed to create listen socket");
    }
    return -1;
  }

  struct sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(port);

  if (bind(listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
    if (err != NULL) {
      *err = new_error_from_errno("failed to bind to port");
    }
    return -1;
  }

  if (listen(listen_socket, CONNECT_BACKLOG) < 0) {
    if (err != NULL) {
      *err = new_error_from_errno("failed to listen");
    }
    return -1;
  }

  return listen_socket;
}

int accept_client_socket(struct error** err, int listen_socket) {
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);
  int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &addr_len);
  if (client_socket < 0) {
    if (err != NULL) {
      *err = new_error_from_errno("failed to accept incoming connection");
    }
    return -1;
  }

  DEBUG_LOG("Received connection from %s:%hu", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

  return client_socket;
}

int tcp_connect(struct error** err, const struct sockaddr_in* remote_addr) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    if (err != NULL) {
      *err = new_error_from_errno("failed to create TCP socket");
    }
    return -1;
  }

  DEBUG_LOG("tcp connecting to %s:%hu", inet_ntoa(remote_addr->sin_addr), ntohs(remote_addr->sin_port));

  if (connect(sock, remote_addr, sizeof(struct sockaddr_in)) < 0) {
    close(sock);
    if (err != NULL) {
      char* msg;
      asprintf(&msg, "failed to connect to %s:%hu", inet_ntoa(remote_addr->sin_addr), ntohs(remote_addr->sin_port));
      *err = new_error_from_errno(msg);
    }
    return -1;
  }

  DEBUG_LOG("connected to server: %s:%hu", inet_ntoa(remote_addr->sin_addr), ntohs(remote_addr->sin_port));

  return sock;
}

int lookup_host_addr(struct error** err, const char* hostname, struct addrinfo** results) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  // TODO: what to do with port number?
  int gai_errno = getaddrinfo(hostname, "http", &hints, results);
  if (gai_errno != 0) {
    if (err != NULL) {
      *err = new_error(append_error_messages("host resolution failed", gai_strerror(gai_errno)), NULL);
    }
    return -1;
  }

  return 0;
}

int tcp_connect_using_hostname(struct error** err, const char* hostname) {
  struct addrinfo *host_addrs, *rp;

  if (lookup_host_addr(err, hostname, &host_addrs) < 0) {
    return -1;
  }

  int sock;
  for (rp = host_addrs; rp != NULL; rp = rp->ai_next) {
    if ((sock = tcp_connect(NULL, (struct sockaddr_in*)rp->ai_addr)) > 0) {
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

  DEBUG_LOG("request: %s", request_line);

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
    die("provide port number as the first argument\n");
  }

  struct error* err;

  int listen_port = parse_port_number(&err, argv[1]);
  if (listen_port < 0) {
    die(format_error_messages(err));
  }

  int listen_socket = tcp_listen(&err, listen_port);
  if (listen_socket < 0) {
    die(format_error_messages(err));
  }
  printf("Listening on port %d\n", listen_port);

  int client_socket = accept_client_socket(&err, listen_socket);
  if (client_socket < 0) {
    die(format_error_messages(err));
  }

  char read_buffer[READ_BUFFER_SIZE];
  int n_bytes_read = read(client_socket, read_buffer, READ_BUFFER_SIZE - 1);
  if (n_bytes_read < 0) {
    die("failed to read from client socket");
  }
  read_buffer[n_bytes_read] = '\0';

  // Trim whitespace from hostname
  char* hostname = read_buffer + n_bytes_read - 1;
  while (isspace(*hostname)) {
    hostname--;
  }
  *(hostname + 1) = '\0';
  hostname = read_buffer;
  while (*hostname != '\0' && isspace(*hostname)) {
    hostname++;
  }

  int server_socket = tcp_connect_using_hostname(&err, read_buffer);
  if (server_socket < 0) {
    die(format_error_messages(err));
  }

  http_get(server_socket, hostname);
  relay_http_response(client_socket, server_socket);

  if (close(client_socket) < 0) {
    die("failed to close client socket");
  }
  if (close(server_socket) < 0) {
    die("failed to close server socket");
  }
  if (close(listen_socket) < 0) {
    die("failed to close listen socket");
  }

  return 0;
}