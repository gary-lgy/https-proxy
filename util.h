#ifndef HTTPS_PROXY_UTIL_H
#define HTTPS_PROXY_UTIL_H

char* hsprintf(const char* fmt, ...);
char* errno2s(int errnum);
void die(const char* message);

#endif  // HTTPS_PROXY_UTIL_H
