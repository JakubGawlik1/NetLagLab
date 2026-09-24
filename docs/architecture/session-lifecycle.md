# Session lifecycle

## Status

The current code implements a user-scoped Session supervisor, starts the
privileged helper, waits for `READY`, directly starts the Workload, serves one
Controller, and reaps the Workload. The accepted target lifecycle moves
Workload and privileged-resource ownership to the helper and adds transactional
setup and cleanup. That target lifecycle is not implemented.

## Responsibility

A Session is one controlled execution of a Workload together with its network
environment, Network Profile, and owned resources from setup through cleanup.
It is successful only after all owned resources have been removed.

The Supervisor coordinates the user-facing lifecycle. The helper is mandatory
and owns privileged setup, the directly managed Workload, privileged runtime
changes, reaping, and cleanup. The optional Controller can observe or request
changes but owns no Session resources.

## Current implementation

The implemented startup path is:

```text
validate XDG_RUNTIME_DIR and session directory
  -> acquire per-user session.lock
  -> start sudo -- netlaglab-helper
  -> connect helper.sock and receive READY
  -> create control.sock
  -> posix_spawnp() the Workload directly from the Supervisor
  -> poll control.sock and waitpid(WNOHANG) for the Workload
```

The per-user `session.lock` prevents two Sessions for that user. It does not
provide host-wide exclusion. The helper launcher is checked only until the
socket connection succeeds; it is not monitored or reliably reaped afterward.
The Supervisor does not send `SHUTDOWN`, await `STOPPED`, or continue reading
helper events after `READY`. Closing `helper.sock` on normal Session teardown
therefore makes the helper report an error.

See the [runtime call map](../runtime_call_map.md) for the exact implemented
functions, descriptors, processes, and incomplete connections.

## Accepted target lifecycle

### Scope and exclusion

- The MVP permits one active Session for the whole computer.
- The existing per-user Supervisor lock remains useful for protecting the
  user-runtime directory and local control endpoint.
- The helper additionally owns a root-owned global host lock, recommended as
  `/run/netlaglab/host.lock`.
- The helper holds the global lock from privileged preflight through the end
  of cleanup. Interrupted-state recovery must run under the same lock.

### Startup transaction

The semantic order is:

1. The Supervisor validates its runtime directory and acquires the per-user
   lock.
2. Before invoking `sudo`, it explains why privileges are needed and that an
   exact scoped firewall exception may later require confirmation.
3. The helper authenticates the Supervisor, acquires the global host lock, and
   performs semantic preflight checks and collision detection.
4. If a firewall mutation is necessary, the helper reports the exact plan and
   the user confirms it through `/dev/tty` before mutation.
5. The helper creates the network and mount environment and applies the
   unrestricted initial Network Profile.
6. The helper launches the Workload with the invoking user's identity and
   execution context.
7. The Session becomes active only after Workload execution has succeeded.

Every step before activation belongs to one transaction. A failure rolls back
all resources created so far and leaves no Workload running. Successful system
calls and semantic precondition checks are sufficient readiness evidence;
startup does not add public ping, DNS, or Internet-connectivity gates.

The exact helper frame that proves successful `exec` and activates the Session
is still protocol design work. It must preserve the accepted distinction
between pre-activation execution failure and an active Workload result.

### Runtime

- The helper is mandatory. Helper loss is an infrastructure failure.
- Supervisor loss makes the helper stop and reap the directly managed
  Workload, clean its owned resources, and exit. It must not remain as an
  orphaned root process.
- Controller detach, EOF, protocol failure, or process failure does not stop
  the Session.
- Only one Controller operation is in flight at a time. Profile status changes
  only after helper acknowledgement.
- Natural Workload exit, a terminal signal handled by `netlaglab run`, and a
  Controller `stop` request all converge on reaping followed by cleanup.

### Stop policy

- Controller `stop` or Supervisor loss sends `SIGTERM` to the directly managed
  Workload PID, waits 5 seconds, then sends `SIGKILL`, reaps, and cleans up.
- On the first terminal Ctrl-C, preserve `SIGINT`; after 5 seconds escalate to
  `SIGTERM`, and after another 5 seconds to `SIGKILL`.
- A second Ctrl-C skips the remaining grace periods and requests immediate
  `SIGKILL`.
- NetLagLab-generated signals target only the directly managed Workload PID,
  not a descendant process group.
- The Workload child uses `PR_SET_PDEATHSIG(SIGKILL)` so unexpected helper
  death does not leave that directly managed process without its owner.

### Result precedence

- A launch failure before activation keeps the conventional `126` result for
  permission or executable-format failure and `127` for a missing program.
- After activation, the Workload's normal exit code or `128 + signal` is
  returned only if helper communication, reaping, launcher reaping, and cleanup
  all succeed.
- Infrastructure failure, unknown privileged state, reaping failure, or
  cleanup failure overrides the Workload result with `125`. Diagnostics should
  still preserve both the Workload outcome and the infrastructure failure.
- A requested stop returns the Workload's actual final status; it is not
  converted to unconditional success.
- `netlaglab attach` returns `0` after correctly observing a clean Session end,
  regardless of the Workload result, and `1` for lost connection, protocol
  failure, or Session failure.

### Helper launcher reaping

The Supervisor always reaps its direct `sudo` launcher. After the helper's
final event it allows 2 seconds for natural launcher exit, then sends
`SIGTERM`, waits another 2 seconds, sends `SIGKILL` if necessary, and finally
performs blocking `waitpid()`. Forced or inconsistent launcher termination is
an infrastructure failure. There is no timeout around the initial interactive
`sudo` prompt.

## Completion contract

Natural Workload exit is not by itself successful Session completion. Success
requires the terminal result to be known, the directly managed Workload and
helper launcher to be reaped, and every NetLagLab-owned resource to be cleaned
up. Detailed resource and descendant semantics are in
[Cleanup and recovery](cleanup-and-recovery.md).

## Open implementation design

- The complete helper protocol state machine and exact activation/final-event
  vocabulary.
- The precise integration of terminal signal handling with the one-threaded
  Supervisor loop while preserving Workload standard input.
- Focused protocol, process, and lifecycle tests, followed by a separately
  authorized privileged happy-path test.
