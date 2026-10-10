# Supervisor-helper protocol

## Status

The lifecycle conversation, start block, explicit standard-descriptor
transfer, stop commands, terminal events, final cleanup result, and typed
Profile Change conversation are implemented. The conversation module owns
stream framing and legal ordering on both sides. Controller dispatch and the
temporary production completion policy are connected; privileged
network-resource operations remain absent.

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

The protocol transports per-setting Profile Changes rather than CLI text or a
complete Network Profile. `ProfileChange` is a shared domain type belonging
with `NetworkProfile`, not a Controller-owned or duplicated wire DTO. The
Controller creates it, the Supervisor-helper protocol transports it, and the
helper realizes it.

The accepted request vocabulary is operation-specific and carries normalized
values:

```text
PROFILE_SET_DELAY OUTBOUND 10
PROFILE_SET_JITTER INBOUND 250
PROFILE_SET_LOSS OUTBOUND 1.5
PROFILE_SET_BANDWIDTH INBOUND 5000
PROFILE_RESET OUTBOUND DELAY
```

Directions are `OUTBOUND` or `INBOUND`; reset settings are `DELAY`, `JITTER`,
`LOSS`, or `BANDWIDTH`. Delay and jitter are nonnegative milliseconds,
bandwidth is positive kilobits per second, and loss is a finite percentage in
the inclusive range `[0, 100]`. The Supervisor serializes loss with
locale-independent `std::to_chars` in general format and
`max_digits10` precision so the helper can recover the exact finite `double`.
The internal loss token may therefore use exponent notation even though the
user-facing Controller grammar does not. The helper requires the complete
token to be consumed and validates the normalized value again.

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
| Helper to Supervisor | `ERROR <safe text>\n` | A generic error terminates the conversation; the dedicated `ERROR PROFILE_STATE_UNKNOWN` case keeps only lifecycle completion legal. Text never includes execution-context values. |

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
- A successful Profile Change produces `PROFILE_OK`.
- `PROFILE_FAILED APPLY_FAILED` is non-terminal and means that applying the
  change failed but the helper proved that it restored the preceding confirmed
  state.
- A malformed, invalid, or out-of-order runtime request is a terminal protocol
  failure reported as `ERROR INVALID_RUNTIME_COMMAND`.
- Failed rollback or an unknown applied state is terminal infrastructure
  failure reported as `ERROR PROFILE_STATE_UNKNOWN`.
- The helper mutates privileged state before sending success.
- The Supervisor updates its public Network Profile only after success.
- A helper operation failure leaves the last confirmed profile visible if the
  helper successfully restored that state.
- Failed rollback or an unknown applied state is terminal infrastructure
  failure.

The helper validates operation legality against its current state. Even though
the Supervisor also holds Session state, root-side validation is required at
the security boundary.

The Supervisor-side conversation owns only wire-level pending state. Starting
a Profile Change is one operation that checks the conversation state,
serializes the request, and records the exact typed change before returning the
frame to the transport. A send failure is terminal conversation loss, so the
state does not need a separate transport rollback path. A `PROFILE_OK` or
`PROFILE_FAILED APPLY_FAILED` frame is legal only while that exact operation is
pending. The resulting typed event carries the stored Profile Change and clears
the pending wire state. The Controller queue and the public Network Profile
remain outside the conversation.

Runtime commands and helper events use variants of typed structures rather
than an enum plus unrelated optional payload fields. A runtime command can
therefore carry either a stop command, the compatibility shutdown command, or
one complete Profile Change. A profile-result event carries both its result and
the exact Profile Change retained by the Supervisor conversation. Lifecycle
events with scalar data remain distinct alternatives. This type-safe protocol
representation does not require the central Session lifecycle event type to
carry profile operations: the production adapter handles profile results and
maps only lifecycle-relevant protocol events into the Session lifecycle.

The helper-side conversation only frames, decodes, validates, and orders typed
runtime commands. It does not invoke `tc` or another privileged mechanism. A
separate shaping adapter eventually receives a decoded Profile Change and
returns exactly one of:

- `applied`;
- `restored_after_failure`;
- `state_unknown`.

The helper conversation maps those results to `PROFILE_OK`,
`PROFILE_FAILED APPLY_FAILED`, and `ERROR PROFILE_STATE_UNKNOWN`, respectively.
While a Profile Change is pending, stop commands remain legal but a second
Profile Change is a terminal `ERROR INVALID_RUNTIME_COMMAND` violation.

`PROFILE_FAILED APPLY_FAILED` is non-terminal: the Supervisor leaves the
confirmed Network Profile unchanged, reports the reversible failure when the
originating Controller can still receive it, and continues with the next
queued command. `ERROR PROFILE_STATE_UNKNOWN` is terminal for profile mutation
and the Session result, but not for lifecycle cleanup. It maps to the dedicated
`profile_state` Session infrastructure failure and Controller outcome code
`PROFILE_STATE`; the conversation still carries stop escalation, the Workload
terminal result, and `CLEANUP_OK` or `CLEANUP_FAILED`.

The production helper routes each valid Profile Change to the Session-owned
Network Environment. It reports `applied` only after `tc` confirms the complete
directional qdisc, `restored_after_failure` only after a bounded rollback
succeeds, and `state_unknown` when the link proof or rollback is unavailable.
The existing conversation preserves stop escalation, Workload-terminal, and
cleanup handling after `state_unknown`.

The helper conversation continues framing input while a Profile Change result
is pending. It retains an incomplete suffix, returns complete stop or
compatibility shutdown commands to the adapter, and rejects a second complete
Profile Change immediately. Completing the privileged operation is a separate
conversation action that consumes `applied`, `restored_after_failure`, or
`state_unknown`, emits the corresponding response, and only then permits the
next Profile Change. The parser performs no privileged work and does not block
socket reads, so the same contract supports a synchronous or future
asynchronous shaping adapter.

The first accepted `STOP TERM`, `STOP KILL`, or compatibility `SHUTDOWN`
command puts the helper runtime conversation into `stopping`. Repeated stop
commands remain legal so the existing escalation can reach `SIGKILL`, but a
later Profile Change is a terminal `ERROR INVALID_RUNTIME_COMMAND` violation.
This rule is enforced independently by the helper even though the Supervisor
control plane also rejects Profile Changes while stopping.

Completing a Profile Change as `state_unknown` puts the helper conversation
into a lifecycle-only failure state. It rejects every later Profile Change but
continues to accept stop escalation and to produce the Workload terminal and
cleanup frames. While the Supervisor channel remains usable, the Supervisor
owns the existing TERM-to-KILL policy; the helper does not start a competing
deadline.

Numeric wire tokens are canonical. Nonnegative integers have no sign or
leading zeroes except the single token `0`. A loss token is accepted only when
parsing consumes the whole token, produces a finite in-range value, and
serializing that value with the accepted `to_chars` rule reproduces the exact
input token. Alternate spellings of the same number are rejected.

`stop` is priority lifecycle control rather than a queued Profile Change. An
accepted stop cancels the pending Controller reply and queued Controller
commands, and the Supervisor may send `STOP` while a Profile Change is still in
flight. This does not promise that a synchronous helper mutation can be
interrupted: the helper may finish it and send its operation result before the
Workload terminal event. The Supervisor consumes such a result for protocol
ordering but does not publish a Profile Change reply after accepting `stop`.

## Terminal event ordering

When the helper recognizes Workload exit, it sends the terminal event and the
Session proceeds to cleanup.

- An acknowledgement for an in-flight profile operation may already have been
  sent. If it was not sent before the terminal event, it is no longer required.
- The terminal event cancels the Supervisor's pending operation and queued
  Controller commands.
- No operation reply may be sent after the terminal event.
- Frames are processed in stream order even when one socket read yields several
  complete frames. A profile result preceding the terminal frame may update the
  confirmed Network Profile and produce its reply; a result after the terminal
  frame cannot produce a Controller reply.
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

## Implemented profile-operation protocol checkpoint

The accepted checkpoint is complete when focused unprivileged tests prove:

- all Profile Change variants and both directions round-trip through the exact
  request vocabulary, including boundary values and exact finite loss values;
- the shared domain operation implements every set and reset, rejects invalid
  constructed values, and never partially mutates its input;
- canonical numeric tokens are accepted while alternate spellings, incomplete
  tokens, overflow, oversized frames, and unknown tokens are rejected;
- the Supervisor conversation atomically records an outbound operation,
  rejects a result without one pending, returns the retained Profile Change in
  its typed result, and accepts a result or terminal event but never a result
  after a terminal event;
- the helper conversation accepts stop while a Profile Change is pending,
  rejects a second Profile Change, maps all three shaping-adapter results, and
  remains correct for fragmented and coalesced stream input;
- the first stop enters helper-side stopping state, repeated stop escalation
  remains legal, and later Profile Changes are rejected;
- `state_unknown` prevents further Profile Changes while preserving stop,
  Workload-terminal, and cleanup framing; and
- all existing lifecycle, start-block, stop, terminal, cleanup, and EOF
  behavior remains covered and unchanged.

The conversation checkpoint is now connected to the Controller control plane.
Valid `set` and `reset` commands cross the typed helper boundary and update
public status only after the helper confirms them. Real `sudo`/PTY and
privileged shaping coverage remains a separately authorized qualification
concern.
