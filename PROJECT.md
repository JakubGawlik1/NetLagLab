# NetLagLab project context

Last verified: 2026-09-25

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
- `netlaglab run -- <program> [arguments...]` transports a bounded execution
  context to the helper, which launches one child without a shell, supervises
  it, and reports its exit result.
- One Supervisor owns the per-user Session, lock, `control.sock`, and at most
  one attached Controller. `attach` supports `help`, `status`, `stop`, and
  `detach`; Controller loss does not stop the application.
- Session paths and Unix sockets are validated for ownership, type, permissions,
  stale entries, peer credentials, and descriptor inheritance.
- The Supervisor starts the helper through `sudo`, authenticates the root peer
  as part of that launcher process tree, and runs the typed lifecycle through
  activation, Workload termination, helper cleanup, and bounded launcher
  reaping. The helper owns the directly managed Workload, a root-owned
  host-wide lock, signalling, and reaping.
- The Workload receives the original bounded argv/environment/cwd snapshot,
  invoking UID/GID/supplementary groups, and explicit standard-descriptor
  semantics. It still runs in the host namespaces.
- Typed outbound and inbound profiles exist, but no network shaping is applied.

## Candidate capabilities

These bullets describe desired capabilities, not milestones or fixed task
boundaries. Their grouping and order should be reconsidered with the user from
current code whenever work begins.

- Create and clean up the network namespace and veth pair through the helper.
- Run the application in the namespace and provide routing, scoped NAT/firewall
  handling, and DNS.
- Apply separate outbound and inbound shaping and allow live profile updates.
- Record the settings timeline as JSON Lines for replay and reports.
- Add a GUI over the same typed operations after the CLI/control path supports
  them.

## Technical references

- `docs/cli.md`: current user-facing CLI behavior.
- `docs/architecture/README.md`: accepted target architecture and evidence map.
- `docs/runtime_call_map.md`: exact current process, socket, and call structure.
- `docs/experiment_01.md`: manual namespace/veth/shaping evidence and cleanup.
