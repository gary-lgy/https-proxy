# Transparent HTTPS Proxy

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
