# Transparent HTTPS Proxy

## Setup
1. Compile the source code
```
make build
```

The executable will be in `./out` directory.

2. Start the proxy
```
./out/proxy <port> <flag_telemetry> <path to blacklist file> [max threads]
```
For example, to start the proxy with the following configurations, 
- listening on port 3000 
- with telemetry enabled
- a blacklist file with name 'blacklist.txt' in `./out` directory
- a maximum number of 8 threads are used  

run `./out/proxy 3000 1 out/blacklist.txt 8` 

Note: The default maximum number of threads is 8 if `max threads` is not specified. The maximum number of threads should be at least 2.

3. Connect to SoC VPN

4. Configure the FireFox settings according to the instructions given in the project doc

5. Start browsing!

## Design

![](./docs/state-transition-diagram.svg)

1. Of the maximum number of threads allowed for the program (by default 8), 1/4 of the threads (or minimally 1 thread) are allocated for **DNS resolution**. The remaining threads are **connection threads** which are responsible for accepting connections and handling the tunneling between client and server.

2. Open a host TCP socket listening at the designated port and wait for incoming connections.
Each connection thread has a `epoll` instance and the listening socket is added to each instance. When incoming connections arrive, all the connection threads will be woken up to accept the connection, but each connection will only be accepted on a single thread. That thread will be responsible for the lifetime of the connection. No additional synchronisation is needed. 

```c
// `handle_connections_in_event_loop`
struct epoll_event event;
// Configure `event.data.ptr` to be NULL when there are events on listening socket
event.data.ptr = NULL;
// Since we will call `accept4` until there are no more incoming connections,
// and edge-triggered is more efficient than level-triggered,
// we can register edge-triggered notification for read events on the listening socket
event.events = EPOLLIN | EPOLLET;

epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listening_socket, &event) 
```  

3. Once an incoming connections is accepted, add the client socket to the handling thread's `epoll` instance and wait for the HTTP CONNECT message from the client socket. 

```c
// `accept_incoming_connections`
struct epoll_accepted_cb* cb = malloc(sizeof(struct epoll_accepted_cb));
cb->type = cb_type_accepted;
cb->conn = conn;

struct epoll_event event;
event.events = EPOLLIN | EPOLLONESHOT;
event.data.ptr = cb;

epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event)
```

3. When the client socket is available for reading, parse the HTTP CONNECT message. 
- If the message is incomplete, read the existing fragment into the buffer and continue waiting for more data to be read from the client socket by registering read event of the client socket
into the `epoll` instance.

```c
// `handle_accepted_cb`
struct epoll_event event;
event.events = EPOLLIN | EPOLLONESHOT;
event.data.ptr = cb;

epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->client_socket, &event)
```

- If the message is well formed and complete, start hostname lookup at the DNS thread.
```c
struct epoll_connecting_cb* cb = malloc(sizeof(struct epoll_connecting_cb));
cb->type = cb_type_connecting;
cb->conn = conn;
cb->failed = false;

struct epoll_event event;
event.events = EPOLLIN | EPOLLONESHOT;
event.data.ptr = cb;

epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cb->asyncaddrinfo_fd, &event)
```

- If the message is malformed, close the connection. 

4. When the asyncaddrinfo lookup result is available, initialize target socket and connect to the target. 
```c
struct epoll_event event;
event.events = EPOLLOUT | EPOLLONESHOT;
event.data.ptr = cb;

epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cb->target_conn_sock, &event)  
```

- If DNS resolution or connection fails, send HTTP 4xx to client.

5. When the connection is completed, send HTTP 200 to client and start the tunnelling.

- Send HTTP 200 to client
```c
- epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_write_fd, &event)  
    -> EPOLLOUT | EPOLLONESHOT  
    -> event.data.ptr = cb  
        -> cb->type = cb_type_tunneling;  
        -> cb->conn = conn;  
        -> cb->is_client_to_target = false;  
        -> cb->is_read = false;  
```

- If there is more to write to target, wait for target socket to become available for writing
```c
- epoll_ctl(epoll_fd, EPOLL_CTL_MOD, target_write_fd, &event) 
    -> EPOLLOUT | EPOLLONESHOT  
    -> event.data.ptr = cb  
        -> cb->type = cb_type_tunneling;  
        -> cb->conn = conn;  
        -> cb->is_client_to_target = true;  
        -> cb->is_read = false; 
``` 
    
- If there is nothing left to write to target, wait for client socket to become available for reading
```c
- epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_read_fd, &event)  
    -> EPOLLIN | EPOLLONESHOT  
    -> event.data.ptr = cb  
        -> cb->type = cb_type_tunneling;  
        -> cb->conn = conn;  
        -> cb->is_client_to_target = true;  
        -> cb->is_read = true;  
``` 

6. Once the tunnelling is setup, the following four events forms the event loop and our proxy serves as the relay agent to relay requests/response between client and server.

- Client socket becomes available to read (i.e. client sends a request to server), 
read the data from the client socket into the client_to_target buffer, wait 
for the target socket to become available for writing so we can send the request to the server.

- Target socket becomes available to write (i.e. we can now relay the request from client to server), write the data in client_to_target buffer to the target socket, wait for the target
socket to become available for reading so we can read the response from the server.

- Target socket becomes available to read (i.e. server replies to client),
read the data from the target socket into the target_to_client buffer, wait 
for the client socket to become available for writing so we can send the response to the client.

- Client socket becomes available to write (i.e. we can now relay data from server to client),
write the data in target_to_client buffer to the client socket, now wait for the client
socket to become available for reading again (i.e. wait for next request)


<!-- ```
struct tunnel_conn {
  // file descriptors
  int client_socket; (set in step 2) // read
  int client_socket_dup; (set in step 5) // write
  int target_socket; (set in step 4) // read
  int target_socket_dup; (set in step 5) // write

  // textual representations of ip/hostname:port for printing
  char* client_hostport; (set in step 2)
  char* target_hostport; (set in step 4)

  // obtained from the CONNECT HTTP message
  char* target_host; (set in step 3)
  char* target_port; (set in step 3)
  char* http_version; (set in step 3)

  // buffers for tunneling
  struct tunnel_buffer client_to_target_buffer; (set in step 2)
  struct tunnel_buffer target_to_client_buffer; (set in step 2)

  // how many directions of this connection have been closed (0, 1, or 2)
  int halves_closed;

  // telemetry
  bool telemetry_enabled; (set in step 2)
  struct timespec started_at; (set in step 2)
  unsigned long long n_bytes_streamed;
};
``` -->

## Why `epoll`?
TODO: what's wrong with one connection per thread? (blocking)

`epoll` is a Linux kernal system call for a scalable I/O event notification mechanism. It monitors multiple file descriptors to see whether I/O is possible on any of them. It is meant to replace the older POSIX `select` and `poll` system calls, to achieve better performance in more demanding applications, where the number of watched file descriptors is large. 

`select` can monitor up to FD_SETSIZE number of descriptors at a time, typically a small number determined at libc's compile time.  
`poll` doesn't have a fixed limit of descriptors it can monitor at a time, but apart from other things, even we have to perform a linear scan of all the passed descriptors every time to check readiness notification, which is O(n) and slow.  
`epoll` has no such fixed limits, and does not perform any linear scans. Hence it is able to perform better and handle a larger number of events.

<!-- ### APIs
- `epoll_create1(int flags)`  
Creates an `epoll` object and returns its file descriptor.

- `epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)`  
Add, modify, or remove entries in the interest list of the epoll instance referred to by the file descriptor `epfd`. The `event` argument describes the object linked to the file descriptor `fd`.

The `struct epoll_event` is defined as:
```
typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;    /* Epoll events */
    epoll_data_t data;      /* User data variable */
};
```

The `data` member of the `epoll_event` structure specifies data that
the kernel should save and then return via `epoll_wait()` when
this file descriptor becomes ready.

The `events` member of the epoll_event structure is a bit mask
composed by ORing together zero or more of the following
available event types:

EPOLLIN  
    The associated file is available for read(2) operations.

EPOLLOUT  
    The associated file is available for write(2) operations.

EPOLLERR  
    Error condition happened on the associated file
    descriptor.

EPOLLONESHOT (since Linux 2.6.2)  
    Requests one-shot notification for the associated file
    descriptor.  This means that after an event notified for
    the file descriptor by epoll_wait(2), the file descriptor
    is disabled in the interest list and no other events will
    be reported by the epoll interface.  The user must call
    epoll_ctl() with EPOLL_CTL_MOD to rearm the file
    descriptor with a new event mask.

EPOLLET  
    Requests edge-triggered notification for the associated
    file descriptor. Edge-triggered mode delivers events only 
    when changes occur on the monitored file descriptor

- `epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)`
Waits for any of the events registered for with `epoll_ctl`, until at least one occurs or the timeout elapses. Returns the orccurred events in `events`, up to `maxvvents` at once. -->

## External Libraries Used

### asyncaddrinfo

- Repository: https://github.com/firestuff/asyncaddrinfo
- Source included under `lib/asyncaddrinfo`
- BSD License

Wraps the blocking `getaddrinfo` call in an asynchronous API.

Internally, it uses a configurable number of worker threads to call `getaddrinfo` and sends the result back
using `socketpair`.

We can add the read end of the `socketpair` into our epoll instances and wait for readability. This allows the server to
keep on serving other requests while `getaddrinfo` is being called concurrently.

We allocate 25% our threads for asyncaddrinfo, i.e., if we run with a maximum of 8 threads, then 2 threads will be
for `asyncaddrinfo` and 6 will run event loops to serve client.

## References

- https://en.cppreference.com/w/c
- https://stackoverflow.com/
- [How to use epoll? - a complete example in C](https://web.archive.org/web/20170427121729/https://banu.com/blog/2/how-to-use-epoll-a-complete-example-in-c/)
- [RFC 7231 Section 4.3.6 CONNECT](https://httpwg.org/specs/rfc7231.html#rfc.section.4.3.6)
- Linux manual pages (e.g., `man socket`, etc)