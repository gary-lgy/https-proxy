#include "poll.h"
#include <errno.h>
#include <malloc.h>
#include <sys/epoll.h>
#include <unistd.h>

#define EPOLL_MAX_EVENTS 64

struct poll {
  int epoll_fd;
};

struct poll* poll_create() {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    return NULL;
  }

  struct poll* p = malloc(sizeof(struct poll));
  p->epoll_fd = epoll_fd;
  return p;
}

void poll_destroy(struct poll* p) {
  close(p->epoll_fd);
  free(p);
}

struct poll_task {
  void* data;
  bool one_shot;
  poll_callback callback;
};

int poll_submit_event(
    struct poll* p,
    int fd,
    void* data,
    uint32_t base_events,
    bool one_shot,
    bool edge_triggered,
    poll_callback callback) {
  /*
   * FIXME:
   * since we only free the task when it's completed, submitting another task before the first one is completed
   * on the same fd results in `task` being leaked.
   */
  struct poll_task* task = malloc(sizeof(struct poll_task));
  task->data = data;
  task->one_shot = one_shot;
  task->callback = callback;

  struct epoll_event event;
  event.data.ptr = task;
  event.events = base_events;
  if (one_shot) {
    event.events |= EPOLLONESHOT;
  }
  if (edge_triggered) {
    event.events |= EPOLLET;
  }

  // try `mod` first, then `add` if `mod` fails
  if (epoll_ctl(p->epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
    if (errno != ENOENT) {
      free(task);
      return -1;
    }

    if (epoll_ctl(p->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
      free(task);
      return -1;
    }
  }

  return 0;
}

int poll_wait_for_readability(
    struct poll* p,
    int fd,
    void* data,
    bool one_shot,
    bool edge_triggered,
    poll_callback callback) {
  return poll_submit_event(p, fd, data, EPOLLIN, one_shot, edge_triggered, callback);
}

int poll_wait_for_writability(
    struct poll* p,
    int fd,
    void* data,
    bool one_shot,
    bool edge_triggered,
    poll_callback callback) {
  return poll_submit_event(p, fd, data, EPOLLOUT, one_shot, edge_triggered, callback);
}

int poll_run(struct poll* p) {
  struct epoll_event events[EPOLL_MAX_EVENTS];
  while (1) {
    int num_events = epoll_wait(p->epoll_fd, events, EPOLL_MAX_EVENTS, -1);
    if (num_events < 0) {
      return num_events;
    }

    for (int i = 0; i < num_events; i++) {
      // TODO: need to check for EPOLLERR?
      struct poll_task* task = events[i].data.ptr;
      task->callback(p, task->data);
      // If it's one-shot, this task will not be used again.
      // Otherwise, subsequent notifications will return the same task pointer.
      if (task->one_shot) {
        free(task);
      }
    }
  }
}
