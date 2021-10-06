#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNECT_BACKLOG 512
#define READ_BUFFER_SIZE 8192

inline __attribute__((always_inline)) void check_err(int status, const char* message);

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "provide port number as the first argument\n");
    exit(EXIT_FAILURE);
  }

  char* endptr;
  int listen_port = strtol(argv[1], &endptr, 10);
  if (*endptr != '\0') {
    fprintf(stderr, "invalid port number: %s\n", argv[1]);
    exit(EXIT_FAILURE);
  }

  printf("Listening on port %d\n", listen_port);

  int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
  check_err(listen_socket, "failed to create listen socket");

  struct sockaddr_in listen_addr;
  socklen_t addr_len = sizeof(listen_addr);
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(listen_port);
  check_err(bind(listen_socket, (struct sockaddr*)&listen_addr, addr_len), "failed to bind to port");

  check_err(listen(listen_socket, CONNECT_BACKLOG), "failed to listen");

  struct sockaddr_in client_addr;
  int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &addr_len);
  check_err(client_socket, "failed to accept incoming connection");
  printf("Received connection from %s:%hu\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

  char read_buffer[READ_BUFFER_SIZE];
  int n_bytes_read = read(client_socket, read_buffer, READ_BUFFER_SIZE - 1);
  check_err(n_bytes_read, "failed to read from client socket");
  //    read_buffer[n_bytes_read] = '\0';

  check_err(write(client_socket, read_buffer, n_bytes_read), "failed to write to client socket");

  check_err(close(client_socket), "failed to close client socket");

  check_err(close(listen_socket), "failed to close listen socket");

  return 0;
}

void check_err(int status, const char* message) {
  if (status < 0) {
    perror(message);
    exit(EXIT_FAILURE);
  }
}