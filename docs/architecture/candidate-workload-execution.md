# Candidate: concentrate Workload execution

## Status

**Exploration candidate — Worth exploring.**

This note preserves a finding from the 2026-09-24 architecture review. It is
not accepted target design, an implementation plan, or a proposed interface.
The ownership and execution rules in
[Workload execution](workload-execution.md) remain authoritative. The final
shape depends on the still-open helper activation handshake.

Dependency category: **in-process plus local-substitutable**. Context capture,
encoding, validation, and display projection can be tested in-process. Actual
execution can use a purpose-built local program.

## Files in the current seam

- [`src/main.cpp`](../../src/main.cpp), where CLI arguments become the current
  Workload input;
- [`src/session.hpp`](../../src/session.hpp) and
  [`src/session.cpp`](../../src/session.cpp), including status rendering,
  `posix_spawnp()`, and exit mapping;
- the future helper-side child setup required by the accepted target design.

## Current friction

The current `run_session(char* const child_arguments[], ...)` type looks small,
but its real interface includes facts not represented by the type:

- the lifetime and null termination of `argv`;
- the original meaning of `argv[0]`;
- global `environ` and PATH lookup;
- the implicit current working directory;
- inherited standard descriptors;
- status escaping and truncation;
- launch-error and terminal-status mapping.

The target helper launch adds lossless argv/environment/cwd transport, user
identity restoration, namespace entry, capability drop, and an activation
result. Without a deeper module, that knowledge would be repeated across CLI,
Supervisor, protocol encoding, helper child setup, and status rendering.

## Deepening direction

Explore a Workload execution module that concentrates capture of the invoking
context, lossless transport representation, safe status projection, program
lookup, child setup, and activation outcome. Privileged mechanics remain
behind the helper seam.

This candidate must preserve the distinction between inert user context and
the root helper's own environment. It must not broaden lifecycle ownership to
Workload descendants.

## Deletion test

The proposed module does not exist yet; the present scattering is the deletion
test outcome. Continuing the accepted target without a deeper module would
make several callers learn the same execution-context and failure rules.

The candidate earns a seam only if one interface serves capture, transport,
helper launch, and observable status without exposing those internal steps.

## Expected benefits

- **Locality:** argv, environment, cwd, identity, lookup, and result rules are
  maintained together.
- **Leverage:** launch, status, transport, and diagnostics reuse one Workload
  meaning.
- **Tests:** context and encoding rules run without root; execution behavior
  uses a local program through the same interface.

## Constraints to preserve

- The helper owns only the directly managed Workload, not its descendants.
- The Workload uses the invoking user's UID, GID, supplementary groups,
  environment, cwd, and standard descriptors.
- Namespace and mount setup precede permanent credential and capability drop.
- User data is never interpreted in the root helper or placed in a shell
  command.
- Bare program lookup uses the transmitted environment; no fallback PATH is
  invented.
- Pre-activation launch failures remain distinguishable from active Workload
  results.

## Questions for later exploration

1. What is the semantic Workload input without exposing raw transport or
   process mechanics?
2. Which representation preserves ordered environment entries and duplicates
   while remaining bounded?
3. Where does safe display projection belong relative to lossless execution
   data?
4. How does activation distinguish `exec` failure from a fast Workload exit?
5. Which behavior is pure in-process logic, and which behavior needs a local
   execution adapter?
