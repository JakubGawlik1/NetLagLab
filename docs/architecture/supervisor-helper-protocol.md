# Supervisor-helper protocol

## Status

The lifecycle conversation, start block, explicit standard-descriptor
transfer, stop commands, terminal events, and final cleanup result are
implemented. The conversation module owns stream framing and legal ordering on
both sides. Profile-mutation operations and privileged network-resource events
remain unimplemented.

## Boundary and responsibilities

The protocol crosses the user-to-root privilege boundary. It is intentionally
small, high-level, stateful, and allowlisted.

- The Supervisor parses Controller and CLI syntax and converts it to typed
  Session operations.
- The helper adapter converts typed operations into protocol frames.
- The helper parses only the allowlisted protocol, validates values and legal
  state, and invokes fixed C++ operations.
- User-derived data is never composed into a shell command.
- Linux, namespace, route, firewall, `tc`, ownership, and rollback details stay
  behind the helper boundary.

The protocol transports per-setting profile deltas rather than CLI text or a
complete Network Profile. A semantic operation can resemble
`SET_OUTBOUND_DELAY 10`, but the complete final wire vocabulary is not yet
settled.

## Semantic lifecycle boundary

The Supervisor-side Session lifecycle consumes only these typed categories
from its conversation adapter:

- activation succeeded;
- activation failed;
- a runtime operation finished;
- the Workload finished;
- privileged cleanup finished;
- the conversation was lost.

Conversation loss is produced locally from EOF, transport failure, malformed
input, or impossible ordering; it is not a helper frame. These categories are
stable lifecycle facts, not proposed wire names. Resource-level events such as
veth, NAT, or qdisc creation remain private to the helper.

## Transport and authentication

`helper.sock` is an `AF_UNIX/SOCK_STREAM` channel. A stream carries bytes, not
messages, so both sides must:

- retain partial input across reads;
- extract all complete frames already present in a buffer;
- handle several frames arriving in one read;
- distinguish EOF from a complete terminal exchange;
- reject malformed or oversized input without losing protocol state.

Ordinary scalar messages use newline framing and the existing 1024-byte limit.
The Workload start block has separate count and decoded-size limits because it
contains argv, environment, and working-directory data.

Authentication is mutual at the process boundary:

- The helper checks `SO_PEERCRED` and accepts only the invoking user represented
  by its validated `SUDO_UID` context.
- The Supervisor must check that the connected peer is the expected root
  helper. It requires root credentials and binds the peer PID to the process
  tree of the `sudo` launcher it created, rather than trusting the pathname or
  `sudo` authentication alone.
- Path ownership, file type, permissions, descriptor inheritance, and stale
  socket cleanup remain defense-in-depth checks; they do not replace peer
  credential checks.

## Current implemented frames

| Direction | Frame | Implemented behavior |
|---|---|---|
| Helper to Supervisor | `READY\n` | Starts the authenticated conversation after the helper acquires the host lock. |
| Supervisor to helper | one descriptor marker plus optional `SCM_RIGHTS` | Explicitly distinguishes transferred non-terminal descriptors, inherited terminal descriptors, and closed descriptors. |
| Supervisor to helper | `START_BEGIN...START_END\n` | Carries bounded argv, environment, and cwd. |
| Helper to Supervisor | `ACTIVE <pid>\n` | Sent only after the exec-success pipe closes on successful `execve`. |
| Helper to Supervisor | `START_FAILED 125|126|127\n` | Reports a pre-activation execution failure. |
| Supervisor to helper | `STOP TERM\n`, `STOP KILL\n` | Signals only the directly managed Workload PID. |
| Supervisor to helper | `SHUTDOWN\n` | Compatibility command currently treated as a TERM stop request. |
| Helper to Supervisor | `WORKLOAD_EXITED <0..255>\n` | Reports a reaped normal Workload result. |
| Helper to Supervisor | `WORKLOAD_SIGNALED <1..127>\n` | Reports a reaped signal result. |
| Helper to Supervisor | `CLEANUP_OK\n`, `CLEANUP_FAILED\n` | Final result after releasing the host lock and cleaning implemented owned state. |
| Helper to Supervisor | `ERROR <safe text>\n` | Terminates the conversation; text never includes execution-context values. |

The implemented state machine allows only `READY`, then activation or start
failure, then (after activation) one Workload terminal result, and finally one
cleanup result. EOF is successful only after the final result and with no
partial trailing frame.

## Workload start block

The accepted one-time context block is logically:

```text
START_BEGIN
CWD <hex-encoded bytes>
ARG <hex-encoded bytes>
ARG <hex-encoded bytes>
ENV <hex-encoded bytes>
START_END
```

The format contract is:

- Encode every source byte as exactly two uppercase hexadecimal digits.
- This is lossless byte encoding, not numeric conversion of an entire value.
- Reject malformed hex and any decoded NUL byte.
- Require exactly one `CWD`, followed by at least one `ARG`, followed by zero
  or more `ENV` entries. Returning to an earlier phase is invalid.
- `ARG[0]` is the original Workload `argv[0]`.
- Preserve environment order and duplicates exactly.
- Limit each decoded value to 128 KiB.
- Limit the total decoded data, including terminating NUL bytes, to 1 MiB.
- Allow at most 4096 `ARG` and 4096 `ENV` entries.
- Start one non-renewing 30-second deadline at `START_BEGIN`; there is no
  deadline before `START_BEGIN`, because the user may still be handling
  consent.

A malformed, incomplete, oversized, or timed-out block is fatal. The helper
sends a safe error that does not echo context data, rolls back, closes the
connection, and causes infrastructure result `125`.

The existing `append_escaped_limited()` function is not suitable for this
block: it is display-only, has no decoder, and may truncate.

## Runtime operation ordering

- The Supervisor permits at most one helper operation in flight.
- No request identifiers are needed while that rule holds.
- Replies and asynchronous events use distinguishable first tokens, such as
  `OK`, `ERROR`, and `EVENT`.
- The helper mutates privileged state before sending success.
- The Supervisor updates its public Network Profile only after success.
- A helper operation failure leaves the last confirmed profile visible if the
  helper successfully restored that state.
- Failed rollback or an unknown applied state is terminal infrastructure
  failure.

The helper validates operation legality against its current state. Even though
the Supervisor also holds Session state, root-side validation is required at
the security boundary.

## Terminal event ordering

When the helper recognizes Workload exit, it sends the terminal event and the
Session proceeds to cleanup.

- An acknowledgement for an in-flight profile operation may already have been
  sent. If it was not sent before the terminal event, it is no longer required.
- The terminal event cancels the Supervisor's pending operation and queued
  Controller commands.
- No operation reply may be sent after the terminal event.
- Helper loss, EOF without the expected terminal exchange, malformed protocol,
  and impossible message ordering are infrastructure failures.

## Shutdown and completion

Controlled completion requires semantic equivalents of:

```text
Supervisor requests shutdown or observes the terminal event
  -> helper stops/reaps the directly managed Workload when necessary
  -> helper cleans every owned privileged resource
  -> helper sends a final result
  -> helper exits
  -> Supervisor reaps the sudo launcher
```

The implemented lifecycle uses the frames listed above. `SHUTDOWN` remains a
compatibility spelling for a TERM request; clean completion is proven by the
Workload terminal frame followed by the cleanup frame.

## Open implementation design

- Exact wire names and encodings for future Network Profile mutations and
  acknowledgements.
- Safe error-reason vocabulary exposed to the Controller without leaking
  environment or privileged host details.
- Privileged integration coverage for the real `sudo`/PTY topology; this
  requires separate explicit authorization.
