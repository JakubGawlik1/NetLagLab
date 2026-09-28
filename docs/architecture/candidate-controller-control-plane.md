# Candidate: deepen the Controller control plane

## Status

**Exploration candidate — Worth exploring.**

This note preserves a finding from the 2026-09-24 architecture review. It is
not accepted target design, an implementation plan, or a proposed interface.
The domain role, command grammar, sequencing rules, and non-ownership contract
in [Controller control plane](controller-control-plane.md) remain
authoritative. Exact response and error frames are still open.

Dependency category: **ports and adapters**. Supervisor and Controller are
both owned by NetLagLab. AF_UNIX is the production adapter; an in-memory
conversation adapter can support focused tests.

## Files in the current seam

- [`src/session.cpp`](../../src/session.cpp), including command parsing,
  response generation, status rendering, acceptance, and disconnect handling;
- [`src/attach_client.cpp`](../../src/attach_client.cpp), including response
  block state, terminal interpretation, and terminal presentation;
- [`src/socket_io.hpp`](../../src/socket_io.hpp) and
  [`src/socket_io.cpp`](../../src/socket_io.cpp).

## Current friction

Controller knowledge is divided between the two endpoints as string literals
and local state:

- command recognition and error responses in the Supervisor;
- status construction and the 8 KiB limit in `session.cpp`;
- help/status block markers and state in `attach_client.cpp`;
- terminal response meaning on both sides;
- disconnect and malformed-response behavior.

The status-size constant is already duplicated. The accepted target adds typed
profile mutations, one in-flight helper operation, stopping state, queued
commands, and terminal cancellation. Adding those rules directly to the
current files would increase knowledge shared across the seam and further mix
conversation semantics with `poll()` and terminal I/O.

## Deepening direction

Explore a deep Controller control-plane module around semantic commands,
bounded status, operation sequencing, and terminal exchange. AF_UNIX transport
and terminal presentation remain adapters rather than places that define the
conversation.

The module must not make the Controller an owner of Session resources. It must
also preserve the one-threaded Supervisor model unless a measured requirement
justifies changing it.

## Deletion test

Without consolidation, each protocol change reproduces knowledge in
`session.cpp` and `attach_client.cpp`. Complexity reappears at both endpoints,
so a shared semantic seam can earn its place.

The seam becomes hypothetical if it only moves string constants while callers
still own sequencing, limits, and terminal meaning. The candidate is useful
only if those rules move behind its interface.

## Expected benefits

- **Locality:** grammar, sequencing, status limits, and terminal rules change
  together.
- **Leverage:** Supervisor and attach client consume the same semantic
  conversation.
- **Tests:** command sequencing, terminal cancellation, malformed responses,
  and bounded status can run without the production poll loop.

## Constraints to preserve

- Controller detach, EOF, or failure does not end the Session.
- Only one Controller is attached at a time.
- Only one helper mutation is in flight at a time.
- Status exposes only the last helper-confirmed Network Profile.
- Workload terminal state cancels pending and queued Controller operations.
- No operation reply follows the terminal event.
- The Controller owns presentation state and descriptors, not Session
  resources.

## Questions for later exploration

1. Which conversation facts must be shared, and which presentation facts stay
   local to the attach process?
2. Where should typed command parsing sit relative to Controller framing?
3. How can bounded status remain one semantic value without exposing its text
   encoding through the interface?
4. How does the module represent pending operations and terminal cancellation
   without duplicating Session lifecycle state?
5. Which endpoint-specific tests become unnecessary once the interface is the
   test surface?
