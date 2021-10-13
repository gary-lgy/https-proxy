#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERRNO_BUF_SIZE 1024

int parse_port_number(const char* raw, unsigned short* port) {
  char* endptr;
  unsigned short parsed = strtol(raw, &endptr, 10);
  if (*endptr != '\0') {
    // the raw string contains unrecognized characters
    return -1;
  }
  *port = parsed;

  return 0;
}

// Returns a heap-allocated string formatted as required.
// TODO: look for memory leaks
char* hsprintf(const char* fmt, ...) {
  va_list args1;
  va_start(args1, fmt);
  va_list args2;
  va_copy(args2, args1);
  size_t len = 1 + vsnprintf(NULL, 0, fmt, args1);
  char* buf = malloc(len * sizeof(char));
  va_end(args1);
  vsnprintf(buf, len, fmt, args2);
  va_end(args2);

  return buf;
}

// Returns a heap-allocated string containing the errno explanation.
char* errno2s(int errnum) {
  char* buf = malloc(ERRNO_BUF_SIZE * sizeof(char));
  char* desc = strerror_r(errnum, buf, ERRNO_BUF_SIZE);
  if (desc != buf) {
    // strerror_r returned a pointer to static memory, copy it into the buffer
    strncpy(buf, desc, ERRNO_BUF_SIZE);
  }

  return buf;
}