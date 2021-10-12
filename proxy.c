#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "connection.h"
#include "http.h"
#include "log.h"
#include "util.h"

#define CONNECT_BACKLOG 512

inline __attribute__((always_inline)) void die(const char* message) {
  fprintf(stderr, "%s\n", message);
  exit(EXIT_FAILURE);
}

int tcp_listen(unsigned short port) {
  int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket < 0) {
    die(hsprintf("failed to create listen socket: %s", errno2s(errno)));
  }

  struct sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(port);

  if (bind(listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
    die(hsprintf("failed to bind listen socket to port: %s", errno2s(errno)));
    return -1;
  }

  if (listen(listen_socket, CONNECT_BACKLOG) < 0) {
    die(hsprintf("failed listen: %s", errno2s(errno)));
    return -1;
  }

  return listen_socket;
}

struct connection_t* await_client_connection(int listen_socket) {
  struct connection_t* conn = init_conn();

  int client_socket = accept(listen_socket, (struct sockaddr*)conn->client_addr, &conn->addrlen);
  if (client_socket < 0) {
    char* errno_str = errno2s(errno);
    DEBUG_LOG("failed to accept incoming connection: %s", errno_str);
    free(errno_str);
    return NULL;
  }

  conn->client_socket = client_socket;
  inet_ntop(AF_INET, &(conn->client_addr->sin_addr), conn->client_hostport, INET_ADDRSTRLEN);
  strcat(conn->client_hostport, ":");
  char client_port[MAX_PORT_LEN];
  sprintf(client_port, "%hu", conn->client_addr->sin_port);
  strcat(conn->client_hostport, client_port);
  DEBUG_LOG("Received connection from %s", conn->client_hostport);

  return conn;
}

int tcp_connect(const struct sockaddr_in* remote_addr) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    return -1;
  }

  if (connect(sock, remote_addr, sizeof(struct sockaddr_in)) < 0) {
    close(sock);
    return -1;
  }

  return sock;
}

int lookup_host_addr(const char* hostname, const char* port, struct addrinfo** results) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  int gai_errno = getaddrinfo(hostname, port, &hints, results);
  if (gai_errno != 0) {
    DEBUG_LOG(hsprintf("target_host resolution failed: %s", gai_strerror(gai_errno)));
    return -1;
  }

  return 0;
}

int tcp_connect_to_target(struct connection_t* conn) {
  struct addrinfo *host_addrs, *rp;

  if (lookup_host_addr(conn->target_host, conn->target_port, &host_addrs) < 0) {
    return -1;
  }

  int sock;
  for (rp = host_addrs; rp != NULL; rp = rp->ai_next) {
    if ((sock = tcp_connect((struct sockaddr_in*)rp->ai_addr)) > 0) {
      // succeeded
      memcpy(conn->target_addr, rp->ai_addr, conn->addrlen);
      break;
    }
  }
  freeaddrinfo(host_addrs);

  if (rp == NULL) {  // No address succeeded
    return -1;
  }

  return sock;
}

/** Read the connect request.
 * Upon return, *read_buffer_ptr will be updated to point to the first character in the buffer that is yet to be
 * processed.
 *
 * Returns the number of characters left in the buffer.
 */

ssize_t handle_http_connect_request(struct connection_t* conn, char** read_buffer_ptr) {
  // TODO: convert to async
  char* read_buffer = conn->client_to_target_buffer;
  ssize_t n_bytes_read = 0;

  while (1) {
    char* next_ptr = read_buffer + n_bytes_read;
    size_t remaining_capacity = BUFFER_SIZE - 1 - n_bytes_read;
    if (remaining_capacity <= 0) {
      break;
    }

    ssize_t n_bytes_read_this_time = read(conn->client_socket, next_ptr, remaining_capacity);
    if (n_bytes_read_this_time == 0) {
      DEBUG_LOG(
          "client %s closed the socket before sending full http CONNECT message, received %d bytes: %s",
          conn->client_hostport,
          n_bytes_read,
          read_buffer);
      return -1;
    } else if (n_bytes_read_this_time < 0) {
      if (errno == EINTR) {
        continue;  // interrupted by signal, try again
      }

      char* errno_desc = errno2s(errno);
      DEBUG_LOG(
          "reading for CONNECT from %s failed: %s, received %d bytes: %s",
          conn->client_hostport,
          errno_desc,
          n_bytes_read,
          read_buffer);
      free(errno_desc);
      return -1;
    }

    n_bytes_read += n_bytes_read_this_time;
    read_buffer[n_bytes_read] = '\0';

    char* double_crlf = strstr(read_buffer, "\r\n\r\n");
    if (double_crlf != NULL) {
      // received full CONNECT message
      if (parse_http_connect_message(read_buffer, conn) < 0) {
        // malformed CONNECT
        DEBUG_LOG("couldn't parse CONNECT message: %s", read_buffer);
        return -1;
      }
      char* next_char = double_crlf + 4;  // skip over the double crlf
      *read_buffer_ptr = next_char;
      return n_bytes_read - (next_char - read_buffer);
    }
  }

  DEBUG_LOG("no CONNECT message until buffer is full, abort");
  return -1;
}

void relay_sockets(struct connection_t* conn, bool client_to_target) {
  int read_sock = client_to_target ? conn->client_socket : conn->target_socket;
  int write_sock = client_to_target ? conn->target_socket : conn->client_socket;
  const char* source_hostport = client_to_target ? conn->client_hostport : conn->target_hostport;
  const char* dest_hostport = client_to_target ? conn->target_hostport : conn->client_hostport;
  char* buffer = client_to_target ? conn->client_to_target_buffer : conn->target_to_client_buffer;

  DEBUG_LOG("relaying data (%s) -> (%s)", source_hostport, dest_hostport);

  while (1) {
    ssize_t bytes_read = read(read_sock, buffer, BUFFER_SIZE);
    if (bytes_read <= 0) {
      if (shutdown(write_sock, SHUT_RDWR) < 0) {
        perror("close after one end closes");
      }
      break;
    }
    DEBUG_LOG("received %zu bytes (%s) -> (%s)", bytes_read, source_hostport, dest_hostport);
    ssize_t bytes_written = write(write_sock, buffer, bytes_read);
    if (bytes_written < 0) {
      break;
    }
    DEBUG_LOG("wrote %zu bytes (%s) -> (%s)", bytes_written, source_hostport, dest_hostport);
  }
  DEBUG_LOG("stopping relay (%s) -> (%s)", source_hostport, dest_hostport);
}

void* relay_sockets_target_to_client_thread_func_wrapper(void* args) {
  struct connection_t* conn = args;
  relay_sockets(conn, false);
  pthread_exit(NULL);
}

void handle_new_connection(struct connection_t* conn) {
  char* next_char;
  ssize_t bytes_remaining = handle_http_connect_request(conn, &next_char);
  if (bytes_remaining < 0) {
    return;
  }

  DEBUG_LOG("received CONNECT request: %s %s:%s", conn->http_version, conn->target_host, conn->target_port);

  int target_socket = tcp_connect_to_target(conn);
  if (target_socket < 0) {
    send_unsuccessful_connect_response(conn);
    DEBUG_LOG("failed to connect to target %s", conn->target_hostport);
    return;
  }
  conn->target_socket = target_socket;
  DEBUG_LOG("connected to %s", conn->target_hostport);

  send_successful_connect_response(conn);

  // send the left-over bytes we read from the client to the server
  if (bytes_remaining > 0) {
    DEBUG_LOG("sending %d left over bytes after CONNECT", bytes_remaining);
    if (write(target_socket, next_char, bytes_remaining) < 0) {
      DEBUG_LOG("failed to send left over bytes from CONNECT");
      return;
    }
  }

  pthread_t worker;
  pthread_create(&worker, NULL, relay_sockets_target_to_client_thread_func_wrapper, conn);
  relay_sockets(conn, true);
  pthread_join(worker, NULL);

  DEBUG_LOG("tunnel (%s) -> (%s) closed", conn->client_hostport, conn->target_hostport);
}

void* handle_new_connection_thread_func_wrapper(void* args) {
  struct connection_t* conn = args;
  handle_new_connection(conn);
  free_conn(conn);
  pthread_exit(NULL);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    die("provide listen port number as the first argument");
  }

  unsigned short listen_port;
  if (parse_port_number(argv[1], &listen_port) < 0) {
    die(hsprintf("failed to parse port number '%s'", argv[1]));
  }

  int listen_socket = tcp_listen(listen_port);
  printf("Listening on port %hu\n", listen_port);

  while (1) {
    struct connection_t* conn = await_client_connection(listen_socket);
    if (conn == NULL) {
      die("failed to accept connection");
      break;
    }

    pthread_t worker;
    pthread_create(&worker, NULL, handle_new_connection_thread_func_wrapper, conn);
  }

  if (close(listen_socket) < 0) {
    die(hsprintf("failed to close listen_socket: %s", errno2s(errno)));
  }

  return 0;
}