#ifndef HTTPS_PROXY_POLL_H
#define HTTPS_PROXY_POLL_H

#include <stdbool.h>

struct poll;

struct poll* poll_create();
void poll_destroy(struct poll* p);
int poll_run(struct poll* p);

typedef void (*poll_callback)(struct poll* p, void* data);

int poll_wait_for_readability(
    struct poll* p,
    int fd,
    void* data,
    bool one_shot,
    bool edge_triggered,
    poll_callback callback);

int poll_wait_for_writability(
    struct poll* p,
    int fd,
    void* data,
    bool one_shot,
    bool edge_triggered,
    poll_callback callback);

#endif  // HTTPS_PROXY_POLL_H
