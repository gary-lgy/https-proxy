#ifndef HTTPS_PROXY_PROXY_SERVER_H
#define HTTPS_PROXY_PROXY_SERVER_H

#include <stdbool.h>

struct proxy_server {
  int listening_socket;
  bool telemetry_enabled;
  char** blacklist;
  int blacklist_len;
};

#endif  // HTTPS_PROXY_PROXY_SERVER_H
