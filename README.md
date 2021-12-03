# Transparent HTTPS Proxy

This is a transparent HTTPS proxy written using Linux `epoll`. It acts as a TCP tunnel between a client and a target
server and relays any data sent in either direction. Since it only speaks TCP and does not understand TLS/HTTP, it will
not terminate TLS connections or modify HTTP requests.

## Setup

1. Compile the source code

```bash
make
```

The executable will be in `./out` directory.

2. Start the proxy

```bash
./out/proxy port enable_stats path_to_blocklist [thread_count]
```

For example, to start the proxy with the following configurations,

- listen on port 3000
- enable stats
- use a blocklist file with name `blocklist.txt` in `./out` directory
- use 8 threads

run `./out/proxy 3000 1 out/blocklist.txt 8`

Note: The default number of threads is 8 if `thread_count` is not specified. At least 2 threads are required (the reason
for this is explained later).

3. Connect to SoC VPN (because `xcne1` and `xcne2` are not publicly accessible)

4. Configure the FireFox settings according to the instructions given in the project doc. Use the IP address of `xcne1`
   or `xcne2` (where you're running the proxy) and the port number you have specified when starting the proxy.

6. Start browsing!

## Terminology

- `client`: the host that requested for a tunnel
- `target`: the host that the client requested to connect to

## Design

### Multiplexed, Non-blocking Network IO

Being the proxy, we need to read from both ends and send any data we receive from one end to the other end.

If we have read all the data from a sender, subsequent attempts to read more bytes from the socket will block the
current thread until more data arrives. Similarly, if we send data to a receiver, and the receiver's TCP buffer fills
up, subsequent attempts to send more bytes will block until the remote buffer has space again.

If a thread is blocked for IO, it cannot process other connections until the IO completes. This stalls all the pending
requests that are yet to be served. We could work around this by creating a new thread for each blocking operation
However, this approach would not scale well when we have many connections open.

Try loading https://www.reddit.com and see how many HTTP requests it makes. On my machine it makes 150 (!) requests in
the first 10 seconds of loading the page, without any user interaction. If each request is served on a new thread, we
would create 150 new threads just to serve the homepage of a single website.

It should be obvious that proxying network traffic is inherently an IO-bound task. The performance of the proxy heavily
depends on how we handle IO in a scalable manner. To do this, we must abandon the blocking and synchronous programming
paradigm and adopt signal-driven or asynchronous IO.

Various operating systems provide tools for us to do this. The Linux kernel provides `select`, `poll`, and `epoll`, all
of which are mechanisms for us to monitor a set of file descriptors to see if IO is possible on any of them. We inform
the kernel what we would like to do with each file descriptor (read or write), and block until at least one of the file
descriptors is available for the operation we requested for. Once the kernel reports that a file descriptor is ready, we
know for sure that a subsequent IO will not block.

- `select` can monitor up to `FD_SETSIZE` number of file descriptors at a time, typically a small number (e.g., 1024).
- `poll` doesn't have a fixed limit of descriptors it can monitor at a time, but requires us to perform a linear scan of
  all the passed descriptors every time to check readiness notification, which is `O(n)` and slow.
- `epoll` is meant to replace the older POSIX `select` and `poll` system calls to achieve better performance in more
  demanding applications, where the number of watched file descriptors is large. It has no such fixed limits, and does
  not perform any linear scans. However, it is Linux specific.

In our implementation, we use `epoll` to perform IO multiplexing. When we need to perform IO on a socket, we don't do it
directly. Instead, we add it to our `epoll` instance and watch it for IO readiness. Only after `epoll` notifies us that
he socket is ready do we perform the IO. Meanwhile, we can service other sockets that are ready. This allows each thread
to handle many connections concurrently even on a single thread.

### Asynchronous DNS resolution

The typical way to perform DNS resolution in C is to call the `getaddrinfo` library function. Unfortunately, this is a
blocking call. In some cases, we observed `getaddrinfo` to block the calling thread for up to 6 seconds when looking up
a domain name that is probably not in the DNS cache. This stalls all the pending tasks on the current thread, including
the data forwarding using `epoll`, producing very user-noticeable delays in other connections handled by the same thread
as well.

To solve this problem, we use a small external library `asyncaddrinfo` (link below) which wraps the
blocking `getaddrinfo` call in an asynchronous API. Internally, it uses a configurable number of worker threads to
call `getaddrinfo` and gives us a file descriptor to receive the call result.

We can conveniently add the file descriptor into our epoll instance and wait for its readability. This allows the thread
to keep on serving other requests while `getaddrinfo` is being called concurrently.

We allocate 25% of our threads to `asyncaddrinfo`, i.e., if we run with 8 threads, then 2 threads will be
for `asyncaddrinfo`. At least one thread must be allocated to `asyncaddrinfo`. This is the reason why the proxy needs at
least 2 threads (the other thread is to run an `epoll` instance and handle IO on sockets).

### Multithreading and Synchronization

On program start, the proxy will create a listening socket and listen on the designated port for incoming connections.
It will then spawn a number of threads, some of which are for `asyncaddrinfo`, and the rest (including the main thread)
are for handling proxy connections and socket IO.

Each connection thread runs its own `epoll` instance and the listening socket is added to each instance. When incoming
connections arrive, all the connection threads will be notified of the socket's readability, and attempt to accept the
connection. However, each connection will only be accepted once by kernel guarantees.

The thread that accepted the connection will then be responsible for the lifetime of the connection. All the subsequent
operations performed on the connection will be done by this thread. More specifically, the thread will only add the
connection socket to its own `epoll` instance and no one else's. When the socket is ready for IO, this thread will be
the only thread to receive the notification. As a result, there will be no race conditions and no additional
synchronisation for the connection are needed.

### Lifecycle of a Connection

As explained above, all the threads will monitor the listening socket for incoming connections.

An accepted connection socket can go through a few states, as shown in the state transition diagram below.

![](./docs/state-transition-diagram.svg)

#### Accepted state

Once an incoming connections is accepted, we add the socket to the handling thread's `epoll` instance and wait for the
HTTP CONNECT message from the client socket.

When the client socket is available for reading, read the bytes sent by the client and try to parse the HTTP CONNECT
message.

- If the message is incomplete, continue waiting for more data to be read from the client socket by registering read
  event of the client socket into the `epoll` instance.
- If the message is well-formed and complete, enter the __connectng__ state and start hostname lookup by submitting a
  lookup job to `asyncaddrinfo`. Add the file descriptor returned by `asyncaddrinfo` and add it to `epoll` to monitor
  its readability.
- If the message is malformed, close the connection.

#### Connecting state

When the `asyncaddrinfo` lookup result is available, initialize target socket and connect to the target.

- If DNS resolution or connection fails, send HTTP 4xx to client and close the connection.

When the target connection is established, enter __tunneling__ state.

- Wait for client socket to become available for sending HTTP 200 to client

- If there is more to write to target, wait for target socket to become available for writing

- If there is nothing left to write to target, wait for client socket to become available for reading

#### Tunneling state

Once the tunnelling is setup, our proxy serves as the relay agent to relay requests/response between client and target.

There are two directions in this tunnel: client to target and target to client. We will have one buffer for each
direction of the tunnel. For example, the client-to-target direction will use the buffer named `to_target`
. The proxy will receive data from the client, place it into the `to_target` buffer, and then send the data to the
target. The opposite goes on for the target-to-client direction and the `to_client` buffer.

For the same buffer, we alternate between sending and receiving. The reason is that once we have read data into the
buffer, we can no longer read anymore until we send the data out to the opposite end, otherwise the previously read data
will be overwritted by subsequent reads before they are sent out, resulting in data loss.

The following 4 events can occur while we're in tunneling state.

__client-to-target direction__

- Client socket becomes available for reading (i.e. client sends a request to target). We read the data from the client
  socket into the `to_target` buffer and then wait for the target socket to become available for writing so we can send
  the request to the target.

- Target socket becomes available for writing (i.e. we can now send the bytes we received from client to target). We
  write the data in
  `to_target` buffer to the target socket. After the write succeeds, wait to read from the client again.

__target-to-client direction__

- Target socket becomes available for reading (i.e. target replies to client). We read the data from the target socket
  into the
  `to_client` buffer. Then, wait for the client socket to become available for writing so we can send the response to
  the client.

- Client socket becomes available for writing (i.e. we can now send the bytes we received from target to client), write
  the data in
  `to_client` buffer to the client socket. Then, wait to read from the target again.

At any time, if either the client connection or the target connection is closed, we close the other connection as well
and terminate the tunnel.

## External Libraries Used

### asyncaddrinfo

- Repository: https://github.com/firestuff/asyncaddrinfo
- Source included under `lib/asyncaddrinfo`
- BSD License

## References

- https://en.cppreference.com/w/c
- https://stackoverflow.com/
- [How to use epoll? - a complete example in C](https://web.archive.org/web/20170427121729/https://banu.com/blog/2/how-to-use-epoll-a-complete-example-in-c/)
- [RFC 7231 Section 4.3.6 CONNECT](https://httpwg.org/specs/rfc7231.html#rfc.section.4.3.6)
- Linux manual pages (e.g., `man socket`, etc)
