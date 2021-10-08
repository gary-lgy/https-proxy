#ifndef HTTPS_PROXY_LOG_H
#define HTTPS_PROXY_LOG_H

void debug_log(const char* filename, int lineno, const char* fn_name, const char* fmt, ...);

#ifndef NDEBUG
#define DEBUG_LOG(...) debug_log(__FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#define DEBUG_LOG(...) (void)0
#endif

#endif  // HTTPS_PROXY_LOG_H