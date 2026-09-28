# Candidate: deepen the Session lifecycle module

## Status

**Exploration complete — Direction accepted.**

This note preserves a finding from the 2026-09-24 architecture review. It is
now resolved as one deep Supervisor-side coordination boundary; it is still not
an implementation plan or authorization to change runtime code. The decision
and its adjacent boundaries are recorded in
[ADR-0001](../adr/0001-coordinate-session-lifecycle-in-the-supervisor.md),
[ADR-0002](../adr/0002-keep-wire-protocols-outside-session-lifecycle.md),
[ADR-0003](../adr/0003-preserve-workload-stdio-and-terminal-signal-semantics.md), and
[ADR-0004](../adr/0004-preserve-execution-and-infrastructure-outcomes.md).
The detailed lifecycle and ownership rules remain authoritative in
[Session lifecycle](session-lifecycle.md),
[Workload execution](workload-execution.md), and
[Cleanup and recovery](cleanup-and-recovery.md).

Dependency category: **mostly local-substitutable**. Scripted semantic adapters
exercise lifecycle decisions deterministically, while local processes, Unix
sockets, and a purpose-built test helper cover launcher integration without
privileged network mutation. Root peer authentication and the privileged happy
path remain separate integration checks.

## Files in the current seam

- [`src/session.cpp`](../../src/session.cpp), especially startup and the
  one-threaded supervision loop;
- [`src/helper_process.hpp`](../../src/helper_process.hpp) and
  [`src/helper_process.cpp`](../../src/helper_process.cpp);
- [`src/helper_protocol.hpp`](../../src/helper_protocol.hpp) and
  [`src/helper_protocol.cpp`](../../src/helper_protocol.cpp).

[`src/helper_main.cpp`](../../src/helper_main.cpp) is an adjacent helper-side
boundary. It owns the other process's command loop and will own Workload and
privileged-resource transitions; those responsibilities do not move into the
Supervisor-side module.

## Current friction

The current helper-process interface exposes three ordered operations:

```text
spawn helper -> connect helper.sock -> wait for READY
```

It also returns launcher PID and socket ownership separately. `run_session()`
must know the order, retain both resources, and decide what each failure means.
The active supervision loop receives neither resource, so it cannot locally
monitor helper death, request controlled shutdown, await final cleanup, or
reap the `sudo` launcher.

The implementation is therefore split at the point where ownership and result
precedence need the strongest locality. Extending the target lifecycle would
make more ordering and cleanup knowledge leak through this interface.

## Accepted deepening direction

Use one deep Supervisor-side Session lifecycle module that coordinates the
transition from transactional startup through active supervision to Session
Outcome, cleanup, and launcher reaping. Its caller uses one blocking operation;
startup, activation, supervision, shutdown, and reaping are not caller-driven
phases. Linux process and socket mechanics remain behind narrow internal seams
that return typed lifecycle events and launcher outcomes.

The module owns only Supervisor-side state and resources. The helper remains
the sole owner of the directly managed Workload, its result, privileged
resources, rollback, and cleanup; the Supervisor owns the `sudo` launcher and
Controller endpoint and learns about the Workload through semantic helper
events.

## Deletion test

Deleting `helper_process` would move helper path resolution, `sudo` spawning,
connection retry, partial framing, launcher checks, and diagnostics into
`session.cpp`. The complexity does not disappear, so the seam earns its place.
The problem is depth: its current interface still exposes the lifecycle steps
and leaves their ownership with the caller.

## Expected benefits

- **Locality:** helper, Workload-result, cleanup, and launcher transitions are
  coordinated through one semantic lifecycle.
- **Leverage:** CLI and Controller behavior consume one semantic Session result
  instead of reconstructing process outcomes.
- **Tests:** startup failure, helper loss, Workload exit, cleanup failure, and
  launcher reaping cross the same interface used by callers.

## Constraints to preserve

- One active Session and one-threaded supervision remain the MVP unless a
  measured requirement justifies a change.
- Controller loss does not end the Session.
- Helper loss, unknown privileged state, failed cleanup, or failed reaping is
  infrastructure result `125`.
- Natural Workload exit is not successful Session completion until cleanup and
  reaping succeed.
- The helper-owned Workload runs with the invoking user's identity and
  execution context.
- User-derived data is never placed in a shell command.

## Resolved exploration decisions

1. The caller uses one blocking lifecycle operation that returns a structured
   Session Outcome only after cleanup and launcher reaping.
2. The Supervisor does not mirror helper resource substeps. Absence of error is
   not activation; an explicit semantic activation event is required.
3. Supervisor-helper framing and Controller grammar remain in separate modules.
   The lifecycle consumes typed operations and events.
4. The one-threaded Supervisor loop observes helper conversation, Controller
   endpoints, and a self-pipe through `poll()`. It reaps only the `sudo`
   launcher; Workload events arrive from the helper.
5. Production adapters still call `poll()`, `waitpid()`, and process APIs.
   Narrow seams translate their results into scriptable lifecycle events and
   launcher outcomes instead of introducing a general Linux syscall facade.
6. Focused lifecycle tests cover clean completion, pre-activation failure, fast
   Workload exit, infrastructure-result precedence, Controller loss, and the
   two-stage Ctrl-C policy. Conversation framing has separate tests.
