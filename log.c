#include <stdarg.h>
#include <stdio.h>
#include <time.h>

// Adapted from
// https://stackoverflow.com/questions/8884335/print-the-file-name-line-number-and-function-name-of-a-calling-function-c-pro
// and
// https://en.cppreference.com/w/c/io/vfprintf
void log_(const char* filename, int lineno, const char* fn_name, const char* fmt, ...) {
  // format the time string
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  char time_buf[31];
  struct tm result;
  gmtime_r(&ts.tv_sec, &result);
  size_t rc = strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &result);
  snprintf(time_buf + rc, sizeof(time_buf) - rc, ".%06ld UTC", ts.tv_nsec / 1000);

  // format the user-provided message
  va_list args1;
  va_start(args1, fmt);
  va_list args2;
  va_copy(args2, args1);
  char msg_buf[1 + vsnprintf(NULL, 0, fmt, args1)];
  va_end(args1);
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args2);
  va_end(args2);

  fprintf(stderr, "%s [%s (%s:%d)] %s\n", time_buf, fn_name, filename, lineno, msg_buf);
}
