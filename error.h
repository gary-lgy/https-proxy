#include <stdlib.h>

#ifndef HTTPS_PROXY_ERROR_H
#define HTTPS_PROXY_ERROR_H

struct error {
  const char* message;
  const struct error* wrapped;
};

struct error* new_error(const char* message, const struct error* wrapped);
struct error* new_error_from_errno(const char* message);
char* format_error_messages(const struct error* err);
char* append_error_messages(const char*, const char*);

#endif  // HTTPS_PROXY_ERROR_H
