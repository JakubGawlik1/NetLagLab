# Session lifecycle

## Status

The lifecycle checkpoint is implemented through one blocking typed boundary.
The helper owns and reaps the directly managed Workload and holds the
host-wide lock through cleanup; the Supervisor owns Controller coordination
and reaps its `sudo` launcher. The helper prepares the fixed local network
environment before Workload launch and explicitly cleans it after Workload
reaping.

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
  -> authenticate the root peer as part of that launcher process tree
  -> helper authenticates and sends READY
  -> transfer standard descriptors plus bounded argv/environment/cwd
  -> helper transaction acquires the host lock and prepares the local topology
  -> child enters the retained Session network namespace and drops privileges
  -> helper fork/execve()s the Workload under the invoking identity
  -> receive ACTIVE and only then publish control.sock
  -> poll helper, Controller, and signal self-pipe
  -> helper reports the Workload result, releases the host lock, and reports cleanup
  -> Supervisor reaps the sudo launcher and removes control.sock
```

The per-user `session.lock` protects the user runtime while the transaction-owned
root host lock provides host-wide exclusion. The lifecycle keeps
reading helper events after `READY`, rejects malformed or impossible ordering,
and returns only after final cleanup and launcher reaping.

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
3. The helper authenticates the Supervisor and accepts the bounded execution
   context; the Network Environment transaction then acquires the global host
   lock and performs semantic preflight checks and collision detection.
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

The Supervisor does not mirror the helper's privileged setup substeps or
resource ledger. Before activation, startup is merely pending; the
implementation may express that through control flow rather than a stored or
public `starting` state. Absence of an error is not activation evidence. Only an
explicit semantic activation event allows the Supervisor to treat the Session
as active.

The helper's close-on-exec error pipe proves successful `exec`; only then does
it send `ACTIVE <pid>`. `START_FAILED 126|127` remains a pre-activation result.

### Runtime

- The helper is mandatory. Helper loss is an infrastructure failure.
- Supervisor loss makes the helper stop and reap the directly managed
  Workload, clean its owned resources, and exit. It must not remain as an
  orphaned root process.
- Controller detach, EOF, protocol failure, or process failure does not stop
  the Session.
- Only one Controller operation is in flight at a time. Profile status changes
  only after helper acknowledgement.
- Unknown or internally inconsistent profile state ends profile mutation but
  keeps the helper conversation available for stop escalation, the Workload
  terminal result, and cleanup.
- Natural Workload exit, a terminal signal handled by `netlaglab run`, and a
  Controller `stop` request all converge on reaping followed by cleanup.

### Supervisor event loop

The one-threaded Supervisor integrates handled signals through a non-blocking,
close-on-exec self-pipe observed by `poll()`. The same loop observes the helper
conversation and Controller endpoints; the nearest stop or launcher-reaping
deadline determines the poll timeout. It may reap only the `sudo` launcher.
Workload activation and termination arrive as semantic helper events, never as
a Supervisor-side `waitpid()` target.

The production adapters still perform the real `poll()`, `waitpid()`, signal,
and process operations. Narrow adapters translate those mechanisms into typed
lifecycle events and launcher outcomes. Deterministic tests provide scripted
events and outcomes at those same seams; the design does not introduce one
general-purpose mock of Linux system calls.

When several descriptors are ready in one poll cycle, the production adapter
handles helper frames, terminal self-pipe markers, a new Controller connection,
and active Controller bytes in that order. It executes each control-plane
action batch before moving to the next source. On the first terminal marker it
updates the control plane to public `stopping` before returning the interrupt
to the blocking lifecycle.

### Stop policy

- Controller `stop` or Supervisor loss sends `SIGTERM` to the directly managed
  Workload PID, waits 5 seconds, then sends `SIGKILL`, reaps, and cleans up.
- The first terminal Ctrl-C reaches the Workload exactly once through the
  terminal and process topology. The Supervisor observes the same signal and
  starts the stop deadline without asking the helper to send another `SIGINT`.
- Five seconds after that first Ctrl-C, the helper sends `SIGTERM`; after
  another 5 seconds it sends `SIGKILL`.
- A second Ctrl-C skips the remaining grace periods and requests immediate
  `SIGKILL`.
- NetLagLab-generated signals target only the directly managed Workload PID,
  not a descendant process group.
- The Workload child uses `PR_SET_PDEATHSIG(SIGKILL)` so unexpected helper
  death does not leave that directly managed process without its owner.
- A `profile_state` failure records that infrastructure failure and starts the
  same TERM-to-KILL path if no stop is active. If stopping is already active,
  the failure neither sends a duplicate initial request nor restarts its
  deadline. In both cases the lifecycle continues through Workload reaping and
  privileged cleanup.

### Result precedence

- A launch failure before activation keeps the conventional `126` result for
  permission or executable-format failure and `127` for a missing program.
- After activation, the Workload's normal exit code or `128 + signal` is
  returned only if helper communication, reaping, launcher reaping, and cleanup
  all succeed.
- Infrastructure failure, unknown privileged state, reaping failure, or
  cleanup failure overrides the Workload result with `125`. Diagnostics should
  still preserve both the Workload outcome and the infrastructure failure.
- Unknown or internally inconsistent profile state is recorded as the
  dedicated `profile_state` infrastructure failure and serialized to a
  Controller as `FAILURE PROFILE_STATE`; it is not collapsed into a generic
  helper-conversation failure.
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

## Focused lifecycle tests

Tests through the blocking lifecycle boundary must cover these observable
contracts:

1. activation, Workload exit, privileged cleanup, and launcher reaping return
   the Workload outcome on a clean path;
2. a pre-activation execution or infrastructure failure leaves no active
   Workload and preserves the `126`, `127`, or `125` distinction;
3. a Workload that exits immediately after successful execution is still
   classified as an activated Workload outcome;
4. helper loss, cleanup failure, or launcher-reaping failure produces
   infrastructure result `125` without discarding an already known Workload
   outcome;
5. Controller loss does not stop the Session, the first terminal Ctrl-C starts
   the grace period, and the second requests immediate `SIGKILL`.
6. unknown profile state records `profile_state`, preserves an existing stop
   deadline, and still preserves the Workload result plus any cleanup failure.

Conversation framing and malformed-input behavior belong to focused tests of
the conversation module rather than being duplicated here. Local unprivileged
process tests cover production socket and launcher adapters. The opt-in
integrated Session qualification covers natural exit, Controller stop,
Supervisor loss, and failed exec through the production CLI, but remains a
separately authorized and unverified host execution step; see
[Network Environment qualification](network-environment.md#integrated-session-qualification).

## Remaining implementation boundary

The lifecycle seam is implemented. The next incomplete responsibility is the
privileged network transaction: namespace/veth/routing/DNS/NAT/firewall/qdisc
setup, its resource ledger, rollback, and persistent recovery where required.
