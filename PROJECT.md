# NetLagLab project context

Last verified: 2026-10-02

## How to use this file

This file gives short, durable context. It is not a binding roadmap, task
tracker, implementation plan, or reason to exclude related work from a task.
The current user request and current evidence determine scope. The code, tests,
and configuration remain the source of truth for implemented behavior.

Update this file only when verified capabilities, stable constraints, important
open questions, or broad product direction change. Define detailed task scope,
order, and acceptance criteria with the user when starting that work.

## Product goal

NetLagLab is an educational C++20/Linux portfolio project that runs one local
application in an isolated network environment and changes its network
conditions live. It should remain understandable and defensible in an
internship interview.

The initial product targets Linux and IPv4, one local application process, one
active session, and one attached controller. Multiple sessions, IPv6, and
cross-platform support are not current goals.

## Implemented

- CMake defines `netlaglab_core`, `netlaglab`, `netlaglab-helper`, and
  GoogleTest-based network-profile tests.
- A private standalone Network Environment library contains a transactional
  coordinator and an unprivileged command runner. Through a uniquely owned
  scripted semantic adapter, one preparation operation exercises the complete
  fixed local setup sequence and returns either a move-only prepared owner or
  a typed failure with ordered rollback failures and an optional residual
  cleanup owner. Explicit cleanup and retry are consuming and bounded; owner
  destruction makes one no-throw best-effort pass. Its production entry point
  acquires and owns the root host lock, validates root execution and a trusted
  fixed-path `ip`, rejects fixed namespace/link names and intersecting host
  IPv4 addresses or non-default routes, and uses a bounded validated
  `RTM_GETROUTE` dump across all host routing tables. The command runner
  executes one exact path with separate arguments and an empty environment,
  enforces an absolute deadline, always reaps its child, and retains at most
  4 KiB of standard-error diagnostics while draining the remainder. The
  production adapter can create the fixed namespace and veth roots, retain an
  exact `nsfs` handle, prove the veth identities with validated `RTM_GETLINK`
  replies on both sides of that handle, and reconcile ambiguous mutation and
  proof-based cleanup outcomes. It then assigns both fixed addresses, brings
  up the host endpoint, namespace loopback, and Session endpoint, and installs
  the namespace default route. Namespace-side commands enter through the exact
  retained handle in bounded short-lived children; successful completion alone
  returns the prepared owner, while configuration failure rolls back the proven
  roots. The helper now prepares the topology before Workload launch, passes a
  capability for entering the exact retained namespace only to the forked
  child, and explicitly cleans prepared or residual ownership after Workload
  reaping. The path has deterministic unprivileged coverage but has not passed
  privileged Session qualification. IPv4 forwarding preflight and scoped nft
  NAT are implemented; host firewall rules remain untouched, so forwarded
  traffic depends on existing host policy. DNS mounts and shaping remain
  incomplete, and privileged TCP/UDP qualification is pending.
- `netlaglab run -- <program> [arguments...]` transports a bounded execution
  context to the helper, which launches one child without a shell, supervises
  it, and reports its exit result.
- One Supervisor owns the per-user Session, lock, `control.sock`, and at most
  one attached Controller. `attach` supports `help`, `status`, `stop`, and
  `detach`; Controller loss does not stop the application. Its pure typed
  `ControllerControlPlane` owns framing, a bounded serialized command queue,
  reply ownership, public running/stopping state, and the last helper-confirmed
  Network Profile. Its typed `set`/`reset` Profile Changes are dispatched
  through independently validating Supervisor/helper conversations. Until a
  real shaping backend exists, the production helper explicitly reports every
  change as restored after failure, so status remains truthful. An attached Controller
  receives the complete typed Session Outcome after Supervisor cleanup and
  reports the Workload result independently from infrastructure failures.
- Session paths and Unix sockets are validated for ownership, type, permissions,
  stale entries, peer credentials, and descriptor inheritance.
- The Supervisor starts the helper through `sudo`, authenticates the root peer
  as part of that launcher process tree, and runs the typed lifecycle through
  activation, Workload termination, helper cleanup, and bounded launcher
  reaping. The helper owns the directly managed Workload, a root-owned
  host-wide lock, signalling, and reaping.
- The Workload receives the original bounded argv/environment/cwd snapshot,
  invoking UID/GID/supplementary groups, and explicit standard-descriptor
  semantics. It enters the exact Session network namespace while retaining the
  host's mount namespace.
- Typed outbound and inbound profiles exist, but no network shaping is applied.

## Candidate capabilities

These bullets describe desired capabilities, not milestones or fixed task
boundaries. Their grouping and order should be reconsidered with the user from
current code whenever work begins.

- Provide production Internet routing, scoped NAT/firewall handling, and DNS
  for the Session network namespace.
- Apply separate outbound and inbound shaping and allow live profile updates.
- Record the settings timeline as JSON Lines for replay and reports.
- Add a GUI over the same typed operations after the CLI/control path supports
  them.

## Technical references

- `docs/cli.md`: current user-facing CLI behavior.
- `docs/architecture/README.md`: accepted target architecture and evidence map.
- `docs/runtime_call_map.md`: exact current process, socket, and call structure.
- `docs/experiment_01.md`: manual namespace/veth/shaping evidence and cleanup.
