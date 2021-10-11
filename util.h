#ifndef HTTPS_PROXY_UTIL_H
#define HTTPS_PROXY_UTIL_H

int parse_port_number(const char* raw, unsigned short* port);

char* hsprintf(const char* fmt, ...);
char* errno2s(int errnum);

#endif  // HTTPS_PROXY_UTIL_H
