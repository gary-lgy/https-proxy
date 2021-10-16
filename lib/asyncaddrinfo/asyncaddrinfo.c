#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "asyncaddrinfo.h"

#ifdef NDEBUG
#define ASSERT(x) do { (void)sizeof(x);} while (0)
#else
#include <assert.h>
#define ASSERT(x) assert(x)
#endif

struct asyncaddrinfo_resolution {
	int return_fd;

	char *node;
	char *service;
	struct addrinfo _hints, *hints;

	int err;
	struct addrinfo *addrs;
};

static size_t asyncaddrinfo_num_threads;
static pthread_t *asyncaddrinfo_threads = NULL;
static int asyncaddrinfo_write_fd;

static void *asyncaddrinfo_main(void *arg) {
	int fd = (int) (intptr_t) arg;
	struct asyncaddrinfo_resolution *res;
	ssize_t len;
	while ((len = recv(fd, &res, sizeof(res), 0)) == sizeof(res)) {
		res->err = getaddrinfo(res->node, res->service, res->hints, &res->addrs);
		int return_fd = res->return_fd;
		res->return_fd = -1;
		ASSERT(send(return_fd, &res, sizeof(res), MSG_EOR) == sizeof(res));
		// Main thread now owns res
		ASSERT(!close(return_fd));
	}
	ASSERT(!len);
	ASSERT(!close(fd));
	return NULL;
}

static void asyncaddrinfo_del(struct asyncaddrinfo_resolution *res) {
	if (res->node) {
		free(res->node);
		res->node = NULL;
	}
	if (res->service) {
		free(res->service);
		res->service = NULL;
	}
	free(res);
}

void asyncaddrinfo_init(size_t threads) {
	ASSERT(!asyncaddrinfo_threads);

	int fds[2];
	ASSERT(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds));
	asyncaddrinfo_write_fd = fds[1];

	asyncaddrinfo_num_threads = threads;
	asyncaddrinfo_threads = malloc(asyncaddrinfo_num_threads * sizeof(*asyncaddrinfo_threads));
	ASSERT(asyncaddrinfo_threads);

	for (size_t i = 0; i < asyncaddrinfo_num_threads; i++) {
		int subfd = fcntl(fds[0], F_DUPFD_CLOEXEC, 0);
		ASSERT(subfd >= 0);
		ASSERT(!pthread_create(&asyncaddrinfo_threads[i], NULL, asyncaddrinfo_main, (void *) (intptr_t) subfd));
	}
	ASSERT(!close(fds[0]));
}

void asyncaddrinfo_cleanup() {
	ASSERT(asyncaddrinfo_threads);
	ASSERT(!close(asyncaddrinfo_write_fd));
	asyncaddrinfo_write_fd = -1;
	for (size_t i = 0; i < asyncaddrinfo_num_threads; i++) {
		ASSERT(!pthread_join(asyncaddrinfo_threads[i], NULL));
	}
	free(asyncaddrinfo_threads);
	asyncaddrinfo_threads = NULL;
}

int asyncaddrinfo_resolve(const char *node, const char *service, const struct addrinfo *hints) {
	int fds[2];
	ASSERT(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds));

	struct asyncaddrinfo_resolution *res = malloc(sizeof(*res));
	ASSERT(res);
	res->return_fd = fds[1];
	if (node) {
		res->node = strdup(node);
		ASSERT(res->node);
	} else {
		res->node = NULL;
	}
	if (service) {
		res->service = strdup(service);
		ASSERT(res->service);
	} else {
		res->service = NULL;
	}
	if (hints) {
		memcpy(&res->_hints, hints, sizeof(res->_hints));
		res->hints = &res->_hints;
	} else {
		res->hints = NULL;
	}
	ASSERT(send(asyncaddrinfo_write_fd, &res, sizeof(res), MSG_EOR) == sizeof(res));
	// Resolve thread now owns res

	return fds[0];
}

int asyncaddrinfo_result(int fd, struct addrinfo **addrs) {
	struct asyncaddrinfo_resolution *res;
	ASSERT(recv(fd, &res, sizeof(res), 0) == sizeof(res));
	ASSERT(!close(fd));

	*addrs = res->addrs;
	int err = res->err;
	asyncaddrinfo_del(res);
	return err;
}
