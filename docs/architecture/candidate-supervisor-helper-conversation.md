# Candidate: deepen the Supervisor-helper conversation

## Status

**Exploration candidate — Strong.**

This note preserves a finding from the 2026-09-24 architecture review. It is
not accepted target design, an implementation plan, or a proposed interface.
The semantic constraints in
[Supervisor-helper protocol](supervisor-helper-protocol.md) remain
authoritative. Its exact state machine and frame vocabulary are still open.

Dependency category: **ports and adapters**. Both processes are owned by
NetLagLab. AF_UNIX is the production adapter; an in-memory adapter can exercise
the conversation deterministically in tests.

## Files in the current seam

- [`src/helper_protocol.hpp`](../../src/helper_protocol.hpp) and
  [`src/helper_protocol.cpp`](../../src/helper_protocol.cpp);
- [`src/helper_process.cpp`](../../src/helper_process.cpp), especially the
  `READY` receive path;
- [`src/helper_main.cpp`](../../src/helper_main.cpp), especially command
  framing and `SHUTDOWN` handling;
- [`src/socket_io.hpp`](../../src/socket_io.hpp) and
  [`src/socket_io.cpp`](../../src/socket_io.cpp).

## Current friction

The current protocol module maps four enum/string values: `READY`, `SHUTDOWN`,
`STOPPED`, and `ERROR`. The actual protocol knowledge lives in callers on both
sides of the privilege seam:

- persistent buffers and partial reads;
- multiple frames received together;
- maximum-size enforcement;
- EOF meaning;
- expected first and terminal messages;
- legal ordering and failure handling.

The module's interface describes tokens, not the conversation. Adding the
accepted start block, activation result, operation acknowledgements,
asynchronous Workload events, and final cleanup would otherwise spread more
state across both executables.

## Deepening direction

Explore a deep protocol-conversation module that owns framing and validates
semantic state transitions. Transport remains an adapter at the seam. Callers
should deal in conversation outcomes rather than raw lines and buffer state.

Do not add another module around the existing token map. Either the existing
module absorbs the protocol knowledge or it is replaced.

## Deletion test

Deleting the current `helper_protocol` module would leave only a few string
comparisons and constants in its callers. Little complexity would reappear.
That makes the current module shallow and argues for replacement or deepening,
not preservation of its present interface.

By contrast, deleting a future conversation module should force framing,
ordering, timeout, and terminal-state logic back into both processes. That is
the depth this candidate needs to earn.

## Expected benefits

- **Locality:** one place defines framing, legal ordering, and terminal state.
- **Leverage:** Supervisor and helper consume the same semantic conversation
  rules.
- **Tests:** partial reads, coalesced frames, oversized data, EOF, invalid
  ordering, and terminal races cross one interface.

## Constraints to preserve

- The helper authenticates the invoking user; the Supervisor authenticates the
  expected root peer.
- `SOCK_STREAM` never supplies message framing.
- Ordinary scalar frames remain bounded; the Workload start block has its own
  count, decoded-size, and timeout constraints.
- At most one runtime operation is in flight in the MVP.
- No operation reply follows a terminal Workload event.
- Unknown or failed rollback state ends the Session with result `125`.
- Root-side validation is mandatory even when the Supervisor already validated
  the same operation.

## Questions for later exploration

1. Which semantic states are shared concepts, and which must remain role-local?
2. Where should framing end and state-transition validation begin?
3. What is the smallest outcome vocabulary needed by the Session lifecycle?
4. Can the in-memory adapter reproduce EOF, partial frames, and event races
   without exposing transport details through the interface?
5. Which existing token-level tests would become redundant after tests at the
   conversation interface?
