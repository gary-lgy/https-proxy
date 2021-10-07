#include "error.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct error* new_error(const char* message, const struct error* wrapped) {
  struct error* err = malloc(sizeof(struct error));
  err->message = message;
  err->wrapped = wrapped;
  return err;
}

struct error* new_error_from_errno(const char* message) {
  return new_error(append_error_messages(message, strerror(errno)), NULL);
}

/**
 * result: `err1: err2: err3`
 */
char* format_error_messages(const struct error* err) {
  size_t total_len = 0;
  for (const struct error* e = err; e != NULL; e = e->wrapped) {
    total_len += strlen(e->message) + 2;
  }
  total_len--;

  char* formatted_message = malloc(total_len * sizeof(char));
  char* next_char = formatted_message;
  const struct error* e;
  for (e = err; e->wrapped != NULL; e = e->wrapped) {
    next_char = stpcpy(next_char, e->message);
    next_char = stpcpy(next_char, ": ");
  }
  stpcpy(next_char, e->message);

  return formatted_message;
}

char* append_error_messages(const char* first, const char* second) {
  char* out;
  asprintf(&out, "%s: %s", first, second);
  return out;
}
