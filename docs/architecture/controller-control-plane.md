# Controller control plane

## Status

The Controller control plane is implemented as one Session-scoped event/action
module. It owns command framing, a bounded queue, reply ownership, the last
helper-confirmed Network Profile, and public running/stopping state. Valid
`set` and `reset` commands are dispatched to the helper and complete only after
its typed result. The production helper applies delay, jitter, and packet loss
through the prepared Network Environment. It confirms successful qdisc
operations, restores the previous qdisc after a recoverable failure, and ends
the Session if it cannot establish the actual state. Bandwidth remains
unsupported.

## Role and ownership

A Controller is an optional interface for observing and changing an active
Session. It is a separate `netlaglab attach` process and owns no Session
resources. Detach, stdin EOF, Controller protocol failure, or Controller
process failure does not stop the Workload or Session.

Only one Controller may be attached at a time. After detach, a later Controller
may attach to the same active Session.

The production adapter owns transport admission. If an active Controller
descriptor already occupies its single slot, the adapter sends the fixed
`ERROR Another controller is already attached.` response to the newly accepted
descriptor and closes it without creating a control-plane event. An admitted
connection becomes the active descriptor before the adapter delivers
`ControllerAttachedEvent`; the control plane then owns `ATTACHED` and every
later operational response. This keeps candidate-descriptor identity out of the
module interface.

The Session owns the last helper-confirmed Network Profile and at most one
in-flight Profile Change. A Controller connection owns only commands that have
not yet been dispatched and the right to receive replies to commands from that
connection. A Profile Change becomes accepted when the Supervisor successfully
sends it to the helper, not merely when the Controller text is parsed.

Disconnecting a Controller discards that connection's undispatched commands
and pending replies. It does not cancel an accepted Profile Change. The helper
result may still update the Session's Network Profile, but the reply is never
delivered to a later Controller. A later Controller observes the resulting
confirmed state through `status` after the in-flight operation completes.

## Accepted command grammar

```text
help
status
set <outbound|inbound> <delay|jitter> <value>
set <outbound|inbound> loss <value>
set <outbound|inbound> bandwidth <value>
reset <outbound|inbound> <delay|jitter|loss|bandwidth>
stop
detach
```

Parsing rules:

- Command, direction, and setting names are exact lowercase tokens.
- Spaces and tabs between tokens are flexible; leading and trailing whitespace
  is ignored.
- An empty line is ignored.
- Extra tokens, unknown units, malformed values, and overflow are rejected.
- The existing 1024-byte Controller command limit remains.

The Controller grammar belongs to the Supervisor. It is not forwarded to the
privileged helper.

`reset` restores only the selected direction/setting to its unrestricted
value; it does not replace the rest of the Network Profile.

## Accepted typed parser seam

`parse_controller_command()` is a pure, side-effect-free Supervisor operation
that receives one complete line without its terminating newline. It recognizes
all six command families and returns exactly one of:

- a typed `ControllerCommand`;
- an ignored empty line;
- a structured `ControllerParseError`.

The concrete result shape is:

```cpp
using ControllerParseResult = std::variant<
    ControllerCommand,
    IgnoredControllerCommand,
    ControllerParseError>;
```

`ControllerParseError` is an `enum class`, and `IgnoredControllerCommand` is an
empty marker type. The interface uses neither exceptions nor output parameters.

`ControllerCommand` distinguishes `help`, `status`, a `Profile Change`, `stop`,
and `detach`. A Profile Change identifies exactly one direction and setting and
contains either a typed `set` value or an explicit `reset` action. It is a
request, not a second requested or pending Network Profile.

The C++ representation makes mismatched settings and value types
unrepresentable. `ProfileChange` is a `std::variant` of `SetDelay`, `SetJitter`,
`SetPacketLoss`, `SetBandwidth`, and `ResetSetting`. Every `set` alternative
contains its direction and normalized value; `ResetSetting` contains its
direction and setting. `ControllerCommand` is a `std::variant` of the four
non-profile command markers and `ProfileChange`.

The parser owns command grammar and the 1024-byte complete-line limit. The
stream conversation uses the same module-owned limit to reject an oversized
partial line before a newline arrives. Framing, socket I/O, dispatch, helper
communication, operation queuing, and response presentation remain outside the
parser. The parser and these types remain in the existing
`controller_control_plane` module rather than introducing a separate shallow
parser module.

The limit counts the raw line bytes before leading or trailing whitespace is
removed and excludes only the terminating newline already consumed by framing.
Before tokenization, the parser also rejects every byte except printable ASCII
and horizontal tab. In particular, NUL, carriage return, other control bytes,
and bytes outside ASCII are invalid input.

Parser errors are structural values rather than ready-to-send Controller text.
The presentation layer maps them to bounded user-facing responses and does not
need the parser to retain or echo the original input. `ControllerParseError`
distinguishes these causes:

- `command_too_long`;
- `invalid_character`;
- `unknown_command`;
- `wrong_argument_count`;
- `invalid_direction`;
- `invalid_setting`;
- `invalid_number`;
- `invalid_unit`;
- `value_out_of_range`;
- `value_overflow`;
- `value_underflow`.

When one line violates more than one rule, the parser uses this deterministic
precedence:

1. `command_too_long`;
2. `invalid_character`;
3. an empty line becomes `IgnoredControllerCommand`;
4. `unknown_command`;
5. `wrong_argument_count`;
6. `invalid_direction`;
7. `invalid_setting`;
8. `invalid_number`;
9. `invalid_unit`;
10. `value_out_of_range`;
11. `value_overflow`;
12. `value_underflow`.

The parser replaced direct command-string comparisons without taking ownership
of transport or Session state. Existing `help`, `status`, `stop`, and `detach`
behavior remains unchanged. A valid Profile Change enters the serialized
helper-dispatch path. `help` advertises directional delay, jitter, packet loss,
and reset commands now that the runtime adapter can apply them. Bandwidth
remains unsupported and is identified in the help text.

### Parser error presentation

An empty line produces no response. `command_too_long` sends its error and
disconnects the Controller, preserving the existing oversized-buffer policy.
Every other parser error sends its response and leaves the Controller
connected. The `attach` client already treats ordinary `ERROR` frames as
non-terminal.

The first checkpoint uses this fixed mapping and never echoes the input line:

| Parser result | Controller response |
|---|---|
| `command_too_long` | `ERROR Command exceeds 1024 bytes.` |
| `invalid_character` | `ERROR Command contains an invalid character.` |
| `unknown_command` | `ERROR Unknown command.` |
| `wrong_argument_count` | `ERROR Wrong number of arguments.` |
| `invalid_direction` | `ERROR Direction must be outbound or inbound.` |
| `invalid_setting` | `ERROR Setting must be delay, jitter, loss, or bandwidth.` |
| `invalid_number` | `ERROR Invalid numeric value.` |
| `invalid_unit` | `ERROR Invalid unit for setting.` |
| `value_out_of_range` | `ERROR Value is outside the allowed range.` |
| `value_overflow` | `ERROR Value is too large.` |
| `value_underflow` | `ERROR Value is too small to represent.` |

## Values and normalization

| Setting | Default unit | Explicit units | Accepted numeric form | Normalized form |
|---|---|---|---|---|
| Delay | `ms` | `ms`, `s` | Nonnegative integer | milliseconds |
| Jitter | `ms` | `ms`, `s` | Nonnegative integer | milliseconds |
| Loss | `%` | `%` | Finite decimal from 0 through 100 | percent |
| Bandwidth | `kbps` | `kbps`, `mbps` | Nonnegative integer | kilobits per second |

Attached and separated suffixes are equivalent: `1s` and `1 s`, or `5mbps`
and `5 mbps`, have the same meaning. `1 mbps` is exactly `1000 kbps`.
Conversion must be exact; NetLagLab rejects overflow and non-exact conversions
instead of rounding. Here, exact conversion refers to integer unit scaling such
as seconds to milliseconds and megabits to kilobits. Packet loss continues to
use the existing finite `double` representation; the parser does not promise
exact binary storage of every accepted decimal fraction.

Normalized delay and jitter must fit in
`std::chrono::milliseconds::rep`. Normalized bandwidth must fit in
`std::uint64_t` and be greater than zero. A value or exact integer unit
conversion that does not fit its target type produces `value_overflow`.

Packet loss is first compared mathematically with the inclusive `[0, 100]`
domain. An in-range decimal is converted to the nearest representable finite
`double`. A nonzero decimal too small to remain nonzero as a `double` produces
`value_underflow`; it is never silently converted to zero. NetLagLab imposes no
additional arbitrary digit-count limit beyond the 1024-byte command limit.
`value_out_of_range` is reserved for domain violations such as loss greater
than 100 or a zero bandwidth limit.

Numeric input uses a locale-independent ASCII grammar:

- delay, jitter, and bandwidth contain one or more decimal digits;
- loss contains one or more decimal digits, optionally followed by `.` and one
  or more decimal digits;
- leading zeroes are accepted;
- signs, exponent notation, decimal commas, digit separators, `.5`, `1.`,
  `nan`, and `inf` are rejected.

For a separated unit, the parser validates the complete numeric token before
the unit token. For an attached unit, the first ASCII letter or `%` begins the
suffix. The preceding nonempty text must satisfy the numeric grammar for the
selected setting. A valid numeric part followed by an unsupported suffix
produces `invalid_unit`; an empty or malformed numeric part produces
`invalid_number`. Consequently, delay values `1.5ms`, `1mss`, and `abcms`
produce `invalid_number`, `invalid_unit`, and `invalid_number`, respectively.

The Supervisor normalizes values at the Controller parser seam before
creating a typed Profile Change. Examples include:

```text
set outbound delay 1 s
set inbound jitter 250ms
set outbound loss 1.5%
set inbound bandwidth 5 mbps
```

## Sequencing

- The Supervisor processes at most one Controller operation at a time.
- `set` and `reset` receive success only after helper confirmation.
- Later ordinary commands wait while a Profile Change is in flight; `status`
  cannot overtake it.
- Each Controller connection may queue at most 32 parsed commands, excluding
  the operation in flight. Parser errors and ignored empty lines are handled
  immediately and do not consume queue capacity. Exceeding the limit produces
  one fixed error and disconnects that Controller. Its undispatched queue is
  discarded, while an accepted Profile Change and the Session continue.
- `stop` is the exception: it cancels the pending Controller reply and queued
  commands, produces `STOPPING`, and enters the lifecycle stop path without
  waiting for the Profile Change result.
- Request identifiers are unnecessary in this serialized MVP.
- Controller disconnect does not cancel an accepted Profile Change.
- A Workload terminal event cancels the pending reply and all queued commands.
  No reply may follow the terminal event.

Helper frames retain their stream order even when several frames arrive in one
socket read. A profile success before a Workload terminal event updates the
confirmed Network Profile and may produce its Controller reply before terminal
processing cancels the remaining queue. If the terminal event comes first, no
later profile reply is published. Correctness never depends on `read()` chunk
boundaries.

Complete Controller lines in one read are also processed in stream order.
`stop` preempts all ordinary commands still waiting when it is reached. An
immediately executable `detach` sends `DETACHED`, disconnects, and ignores the
remaining bytes. A `detach` already queued behind an in-flight Profile Change
may instead be cancelled by a later `stop`. Queue overflow disconnects as soon
as the overflowing command is reached, so later bytes cannot rescue the
connection or request a stop.

A new Controller may attach while a Profile Change from a disconnected
Controller remains in flight. Its commands wait behind that operation. The
result updates the Session but produces no reply for the replacement
Controller, after which the replacement's queue continues. The implementation
tracks reply ownership with a private connection generation; it does not expose
request or connection identifiers on either wire protocol.

### Profile-operation responses

The control plane uses the following exact bounded responses:

| Outcome | Controller response | Connection |
|---|---|---|
| Helper confirmed the change | `PROFILE_CHANGED` | remains connected |
| Helper restored the preceding profile | `ERROR Profile change could not be applied; previous profile remains active.` | remains connected |
| `set` or `reset` while stopping | `ERROR Session is stopping.` | remains connected |
| More than 32 queued commands | `ERROR Too many queued commands.` | disconnects after the send attempt |

After `restored_after_failure`, the confirmed Network Profile remains unchanged
and processing continues with the next queued command. If the originating
Controller disconnected or `stop` already cancelled its reply, the result is
consumed only to preserve helper-protocol ordering.

## Profile and status semantics

Status exposes only the last helper-confirmed current Network Profile. There is
no separate requested or pending profile. While a Profile Change is in flight,
status would still represent the previous confirmed state, but serialization
prevents an attached `status` command from overtaking that Profile Change.

The status shaping flag starts as `shaping: not applied`. The first
helper-confirmed `PROFILE_OK` changes it to `shaping: applied`. A reversible
failure preserves the flag as well as the preceding profile: it remains `not
applied` before any success and `applied` after a previously confirmed success.
Resetting the last impairment leaves shaping applied with an unrestricted
profile.

Target status contains:

- Session state: `running` or `stopping`;
- directly managed Workload PID;
- program and arguments, safely escaped and bounded for display;
- outbound and inbound values from the last confirmed Network Profile.

Environment data is never included in status.

## Stop and terminal responses

- An accepted `stop` request produces `STOPPING` and leaves the Controller
  connected for the final result.
- Acceptance immediately changes the public Session state to `stopping`,
  preempts queued commands, and cancels the reply for any in-flight Profile
  Change without cancelling the helper operation itself.
- Requesting the lifecycle stop is an infallible local adapter action and is
  executed before the `STOPPING` send. Failure to notify the Controller
  therefore disconnects only that Controller and cannot undo an accepted stop.
- The first terminal Ctrl-C also changes the public state to `stopping`
  immediately, including during the initial observation grace period before a
  helper `STOP TERM` is required. The adapter delivers `SessionStoppingEvent`
  to the control plane before returning the corresponding terminal-interrupt
  event to the blocking lifecycle.
- While stopping, `help`, `status`, and `detach` remain available. `set` and
  `reset` receive a fixed stopping error. Repeated `stop` is idempotent and
  produces `STOPPING` without accelerating the lifecycle timeout; immediate
  escalation remains the second-terminal-Ctrl-C policy.
- After successful reaping and cleanup, the Controller receives either
  `SESSION_ENDED EXIT <code>` or `SESSION_ENDED SIGNAL <n>`.
- Infrastructure failure produces `SESSION_FAILED <safe-reason>`.
- `netlaglab attach` returns `0` after a correctly observed clean Session end,
  independent of the Workload result.
- It returns `1` for connection loss, protocol failure, or Session failure.
- `netlaglab run` separately returns the Session result defined in
  [Session lifecycle](session-lifecycle.md).

The stop policy and signal escalation are owned by the Session/helper
lifecycle, not by the Controller client.

## Accepted module seam

`ControllerControlPlane` is one deep Session-scoped module replacing the
socket-bound `ControllerConversation`; a second state holder is not layered
beside it. Its interface accepts Controller bytes and disconnects, helper
profile results, stopping transitions, and terminal Workload events. It returns
ordered typed actions for the production adapter to execute: send Controller
text, dispatch a typed Profile Change to the helper, disconnect the Controller,
request lifecycle stop, or fail the Session.

The public state-machine interface has one operation:

```cpp
std::vector<ControlPlaneAction> handle(ControlPlaneEvent event);
```

`ControlPlaneEvent` has typed alternatives for admitted Controller attachment,
received bytes, ordinary Controller disconnection, `ControllerWriteFailedEvent`,
`ProfileChangeResultEvent`, `ProfileStateUnknownEvent`, Session stopping, and a
terminal Workload event.
`ControlPlaneAction` has typed alternatives for sending Controller text,
dispatching a `ProfileChange`, disconnecting the Controller, requesting Session
stop, and terminating with `profile_state`. The constructor receives immutable
status context containing the Workload PID and copied argument strings. Queue,
profile, connection-generation, and shaping-state getters are deliberately not
part of the interface.

The module owns Controller command framing and operational presentation, its
bounded typed queue, reply association, the confirmed Network Profile, and the
public running/stopping state. `ProductionLifecycleAdapter` retains descriptor
and `poll()` ownership and executes the returned actions. The central blocking
Session lifecycle continues to consume only lifecycle-relevant events; profile
results do not expand its general event type unless they terminate the Session.

The final typed `SessionOutcome` remains outside this module. A terminal
Workload event makes the control plane cancel pending replies and queued
commands. After helper cleanup, launcher reaping, and Supervisor cleanup, the
existing `controller_session_outcome` module serializes the final result through
`LifecycleAdapter::publish_outcome()`. This preserves the already implemented
completion seam instead of coupling Controller command processing to cleanup.

Tests exercise the same event-in/action-out interface used by production. The
existing pure parser remains directly testable, but socket tests do not reach
past the control-plane interface to assert private queue representation.

The production adapter executes each returned action batch synchronously and
strictly in order. Before it delivers any later external event, it either
finishes the whole batch or immediately feeds the failed action back to the
control plane. The module may therefore infer that the preceding batch
completed when the next unrelated event arrives.

An action batch places infallible local lifecycle requests first, Controller
sends next, and at most one helper Profile Change dispatch last. A Controller
write failure is fed back as a distinct event rather than an ordinary
disconnection. It closes that Controller, discards the rest of its batch and
queue, and cancels a Profile Change dispatch scheduled later in that aborted
batch. It cannot roll back a Network Profile already confirmed before the send
or cancel a Profile Change dispatched by an earlier completed batch. Failure of
the last helper-dispatch action is terminal helper-conversation failure.

Failure to send text immediately before a planned Controller disconnect still
affects only that Controller. The adapter does not attempt compensating writes
or execute later actions from a failed batch.

### Production event precedence

When one `poll()` result marks several sources ready, the adapter processes
them deterministically in this order:

1. all complete helper frames already returned by the helper conversation;
2. terminal markers drained from the signal self-pipe;
3. one newly accepted Controller connection;
4. bytes from the active Controller.

Each resulting action batch is executed or failed back synchronously before the
next source is handled. Ordering within either byte stream remains framing
order. Consequently, a helper acknowledgement ready with Ctrl-C may confirm its
Profile Change first, while Ctrl-C ready with a new Controller command changes
the public state to `stopping` before that command is interpreted. A terminal
Workload frame prevents later Controller command replies in the same poll
cycle.

## Accepted control-plane checkpoint

This checkpoint connects the bounded Controller queue, Supervisor dispatch,
helper results, the confirmed Network Profile, and public running/stopping
status without implementing `tc` shaping. Until the runtime shaping backend
exists, the production helper must explicitly complete every valid Profile
Change as `restored_after_failure`; it must not silently drop the command or
leave the operation pending. The successful `applied` path is exercised through
an injected in-memory adapter in focused tests.

Consequently, this checkpoint proves production framing, dispatch, ordering,
failure recovery, disconnect handling, and status ownership, but does not claim
that a real Session can apply network conditions. `help` continues to omit
`set` and `reset`. Because its temporary production adapter never reports
`applied`, production status continues to say `shaping: not applied` until the
runtime shaping backend can produce a real successful acknowledgement.

A helper `PROFILE_STATE_UNKNOWN` event, or a local inability to apply the exact
Profile Change carried by `PROFILE_OK`, means that the privileged state can no
longer be represented truthfully. The control plane then emits a terminal
profile-state failure and accepts no further commands. The helper conversation
remains lifecycle-capable: the Session records `profile_state`, starts or
continues its existing stop escalation without restarting a running deadline,
and still consumes the Workload terminal and cleanup results. `SessionOutcome`
maps the dedicated failure to exit status 125 and serializes it to an attached
Controller as `FAILURE PROFILE_STATE`, while preserving any Workload result and
additional cleanup failure.

## Focused acceptance matrix

The checkpoint is tested in layers through the same interfaces used by
production. It does not introduce a general Linux-system-call mock or inspect
the control plane's private queue, profile, or connection-generation state.

| Test surface | Required observable contracts |
|---|---|
| Existing parser and Network Profile domain | Retain the complete grammar, normalization, validation, and atomic application coverage without duplicating their value matrix in control-plane tests. |
| `ControllerControlPlane::handle()` | Initial attach/help/status behavior; partial and coalesced framing; queue ordering; `applied` and `restored_after_failure`; the 32-command bound; ordinary disconnect and replacement Controller ownership; Controller-write failure; stop and public stopping state; Workload-terminal races; and terminal `profile_state`. |
| Supervisor/helper conversation | The first stop blocks later Profile Changes while repeated stop escalation remains legal; `state_unknown` blocks profile mutation but preserves stop, Workload-terminal, and cleanup traffic. |
| Blocking Session lifecycle | `profile_state` starts normal stop escalation or preserves an existing stop deadline, and the final outcome retains a known Workload result plus any cleanup failure. |
| Controller Session Outcome | `PROFILE_STATE` round-trips through serialization and parsing and is reported by `netlaglab attach` as Session failure. |
| Production action executor | One focused unprivileged `socketpair` test uses the real executor to prove action order, helper-frame dispatch, Controller-write-failure feedback, and local lifecycle-stop generation. |

The old socket-bound `ControllerConversation` behavior tests are replaced by
tests through the event-in/action-out interface. The pure parser tests remain
because parsing is a distinct contract. The adapter test uses local sockets as
stand-ins for the real descriptors; it does not justify a new general-purpose
port or a privileged helper run.

Validation builds `netlaglab`, `netlaglab-helper`, and every affected test
target. Focused Controller, helper-protocol, lifecycle, and Controller-outcome
CTest selections run first. The complete CTest suite then runs because this
checkpoint changes shared types and production integration. Real `sudo`, PTY,
namespace, qdisc, and shaping behavior remain part of the separately authorized
privileged qualification gate.

## Implementation checkpoints

Implementation was delivered test-first in three reviewable checkpoints. Each
checkpoint built its affected production consumers and passed focused tests
before the next one began.

### 1. Profile-state lifecycle foundation

- Add `InfrastructureFailure::profile_state`, its diagnostic, and the
  `PROFILE_STATE` Controller outcome mapping.
- Make helper-side stopping reject later Profile Changes while retaining
  repeated stop escalation.
- Keep a `state_unknown` conversation lifecycle-capable and teach the blocking
  lifecycle to record the failure and start or continue normal stop escalation.
- Make the production helper immediately complete every Profile Change as
  `restored_after_failure` until a real shaping adapter exists.
- Run focused helper-protocol, lifecycle, and Controller-outcome tests. Existing
  production Controller behavior remains unchanged at this stop.

### 2. Event-action replacement preserving current behavior

- Replace `ControllerConversation` with `ControllerControlPlane`, its typed
  events and actions, and the production action executor.
- Preserve the implemented `help`, `status`, `stop`, `detach`, parser-error, and
  temporary Profile-Change-unavailable behavior.
- Integrate single-slot transport admission, synchronous action-failure
  feedback, deterministic poll-source precedence, and immediate public
  `stopping` after the first terminal Ctrl-C.
- Replace the old socket-bound behavior tests with tests through `handle()` and
  add the focused production-executor `socketpair` test. Remove the old state
  holder instead of layering both designs.

### 3. Profile control-plane integration

- Enable the 32-command queue, helper dispatch, reply ownership across
  disconnect and replacement attachment, and serialized queue progress.
- Consume `applied` and `restored_after_failure`, update the confirmed Network
  Profile and shaping flag only after success, and preserve the previous state
  after reversible failure.
- Integrate stop, terminal Workload, Controller-write-failure, helper-dispatch-
  failure, and `profile_state` paths.
- Run the complete focused acceptance matrix, then the full CTest suite, and
  update current-state documentation. Stop without committing, pushing, or
  running a privileged qualification unless separately requested.

## Current-to-target gap

The Supervisor now implements the bounded event/action control path, helper
Profile Change requests, confirmed Network Profile updates, and public stopping
state. The remaining gap is privileged realization: production deliberately
returns `restored_after_failure`, so no qdisc state changes and `help` still
omits `set` and `reset`.

## Implemented parser checkpoint

The parser checkpoint provides:

- parser tests cover every command type, both directions, every setting,
  `set` and `reset`, whitespace, units, normalization, and every parse error;
- focused regression cases cover 1024/1025-byte lines, an invalid byte,
  overflow, underflow, range rejection, attached-suffix classification, and
  error precedence;
- control-plane tests cover partial framing, fixed error responses, queueing,
  connection ownership, and disconnect after an oversized command;
- valid `set` and `reset` commands are serialized through helper dispatch and
  receive the explicit reversible-failure result in production;
- existing `help`, `status`, `stop`, and `detach` behavior remains unchanged;
- `docs/cli.md`, the runtime call map, and the dated MVP audit are updated after
  implementation so their current-state claims remain accurate.

## Implementation-readiness status

The current grill has resolved state ownership, queueing, partial action
failure, final-outcome ownership, stopping, and unknown-profile-state cleanup.
The focused acceptance matrix and three implementation checkpoints are fixed
above. The decision frontier is empty, but implementation must not begin until
the user explicitly confirms the shared understanding and authorizes it. This
section is not authorization to broaden the checkpoint into runtime shaping.
