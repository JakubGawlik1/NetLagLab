# Session architecture

NetLagLab's current session consists of three processes:

- the supervisor is the `netlaglab run` process;
- the application is the child started by the supervisor;
- the optional controller is a separate `netlaglab attach` process.

The application keeps the standard streams of the terminal that started `run`. The controller is
only a remote control in another terminal; detaching or terminating it does not stop the
application.

## Socket roles

The supervisor creates a Unix `SOCK_STREAM` listening socket at
`$XDG_RUNTIME_DIR/netlaglab/control.sock`. This socket exists for the whole session and receives
connection requests. It does not carry controller commands itself.

Each successful `connect()` from an attach client causes the supervisor's `accept4()` to create a
separate accepted socket. The accepted socket and the client's anonymous socket are the two ends
that carry commands and responses. Closing either end disconnects that controller. The listening
socket remains available for a later attach.

The stream transports bytes and does not preserve message boundaries. NetLagLab therefore uses
newline-terminated commands and explicitly marked multi-line responses. Both processes retain
incomplete input until a complete line is available.

## One-threaded supervisor

The supervisor has little concurrent work: it watches the listening socket, at most one accepted
client socket, and one application process. A single thread keeps ownership and cleanup direct and
avoids synchronization between threads.

`poll()` waits for socket activity for at most 100 ms. After each iteration,
`waitpid(..., WNOHANG)` checks the application without blocking. This bounds the delay between the
application ending and the supervisor noticing it while still allowing socket traffic to be
handled in the same thread. A more complex design using worker threads or `pidfd` should only be
considered if measurement or new requirements show that this bounded polling is inadequate.

## Lifetime and safety

The supervisor holds an exclusive non-blocking `flock()` on `session.lock`. The file itself may
remain in the runtime directory, but the kernel lock exists only while the supervisor holds its
open descriptor. A second `run` therefore cannot start another active session.

Descriptors are created with close-on-exec (`O_CLOEXEC` or `SOCK_CLOEXEC`). The application child
must not inherit the lock or control sockets when `posix_spawnp()` replaces its process image. If
it inherited the lock's open file description, the session could appear active after the
supervisor exited.

RAII objects own the descriptors in C++ and call `close()` on every normal and error return path.
The supervisor also owns the `control.sock` filesystem entry and removes it during controlled
cleanup. After an abrupt supervisor death, a later `run` may remove the stale socket only after it
has acquired the session lock and verified the entry's type and owner.
