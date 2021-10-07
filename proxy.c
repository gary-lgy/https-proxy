#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "error.h"
#include "http_connect.h"
#include "log.h"

#define CONNECT_BACKLOG 512
#define READ_BUFFER_SIZE 8192

inline __attribute__((always_inline)) void die(const char* message) {
  fprintf(stderr, "%s\n", message);
  exit(EXIT_FAILURE);
}

int parse_port_number(struct error** err, const char* raw, unsigned short* port) {
  char* endptr;
  *port = strtol(raw, &endptr, 10);
  if (*endptr != '\0') {
    if (err != NULL) {
      char* msg;
      asprintf(&msg, "invalid port number: %s\n", raw);
      *err = new_error(msg, NULL);
    }
    return -1;
  }

  return 0;
}

int tcp_listen(struct error** err, unsigned short port) {
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

  DEBUG_LOG("tcp connected to %s:%hu", inet_ntoa(remote_addr->sin_addr), ntohs(remote_addr->sin_port));

  return sock;
}

int lookup_host_addr(struct error** err, const char* hostname, const char* port, struct addrinfo** results) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  int gai_errno = getaddrinfo(hostname, port, &hints, results);
  if (gai_errno != 0) {
    if (err != NULL) {
      *err = new_error(append_error_messages("host resolution failed", gai_strerror(gai_errno)), NULL);
    }
    return -1;
  }

  return 0;
}

int tcp_connect_to_target(struct error** err, const struct http_connect_request* request) {
  struct addrinfo *host_addrs, *rp;

  if (lookup_host_addr(err, request->host, request->port, &host_addrs) < 0) {
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

int handle_connect_request(
    struct error** err,
    int sock,
    char* read_buffer,
    size_t buffer_len,
    struct http_connect_request* result) {
  // TODO: convert to async
  size_t n_bytes_read = 0;

  while (1) {
    char* next_ptr = read_buffer + n_bytes_read;
    size_t remaining_capacity = buffer_len - 1 - n_bytes_read;

    ssize_t n_bytes_read_this_time = read(sock, next_ptr, remaining_capacity);
    if (n_bytes_read_this_time < 0) {
      if (errno == EINTR) {
        // interrupted by signal, try again
        continue;
      }
      if (err != NULL) {
        *err = new_error_from_errno("failed to read from client socket");
      }
      return -1;
    }

    n_bytes_read += n_bytes_read_this_time;
    read_buffer[n_bytes_read] = '\0';
    char* double_crlf = strstr(read_buffer, "\r\n\r\n");
    if (double_crlf != NULL) {
      // received full CONNECT message
      if (parse_http_connect_message(read_buffer, result) < 0) {
        // malformed CONNECT
        return -1;
      }
      return 0;
    }
  }
}

void relay_sockets(int read_sock, int write_sock) {
  DEBUG_LOG("relaying data from %d to %d", read_sock, write_sock);
  char buffer[READ_BUFFER_SIZE];
  while (1) {
    ssize_t bytes_read = read(read_sock, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
      return;
    }
    DEBUG_LOG("received %zu bytes from fd %d", bytes_read, read_sock);
    ssize_t bytes_written = write(write_sock, buffer, bytes_read);
    DEBUG_LOG("wrote %zu bytes to fd %d", bytes_written, write_sock);
  }
  DEBUG_LOG("stopping relay from %d to %d", read_sock, write_sock);
}

void* thread_func(void* args) {
  int* sockets = args;
  relay_sockets(sockets[0], sockets[1]);
  pthread_exit(NULL);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    die("provide port number as the first argument\n");
  }

  struct error* err;

  unsigned short listen_port;
  if (parse_port_number(&err, argv[1], &listen_port) < 0) {
    die(format_error_messages(err));
  }

  int listen_socket = tcp_listen(&err, listen_port);
  if (listen_socket < 0) {
    die(format_error_messages(err));
  }
  printf("Listening on port %hu\n", listen_port);

  int client_socket = accept_client_socket(&err, listen_socket);
  if (client_socket < 0) {
    die(format_error_messages(err));
  }

  char read_buffer[READ_BUFFER_SIZE];
  struct http_connect_request request;
  // TODO: some bytes are discarded, should forward them to the target server
  handle_connect_request(NULL, client_socket, read_buffer, sizeof(read_buffer), &request);

  DEBUG_LOG("received CONNECT request: %s %s:%s", request.http_version, request.host, request.port);

  int server_socket = tcp_connect_to_target(&err, &request);
  if (server_socket < 0) {
    die(format_error_messages(err));
  }

  send_successful_connect_response(client_socket, &request);

  int client_to_server[2] = {client_socket, server_socket};
  int server_to_client[2] = {server_socket, client_socket};

  pthread_t worker1, worker2;
  pthread_create(&worker1, NULL, thread_func, server_to_client);
  pthread_create(&worker2, NULL, thread_func, client_to_server);
  pthread_join(worker1, NULL);
  pthread_join(worker2, NULL);

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