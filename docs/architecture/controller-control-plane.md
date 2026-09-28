# Controller control plane

## Status

The current Controller supports `help`, `status`, `stop`, and `detach` over
`control.sock`. Its pure typed parser also recognizes, validates, and
normalizes `set` and `reset` Profile Changes. Until helper dispatch exists,
valid Profile Changes receive `ERROR Profile changes are not available yet.`
and do not alter status. `stop` enters the implemented helper-owned termination
policy. Status always shows an unrestricted default profile and `shaping: not
applied`.

## Role and ownership

A Controller is an optional interface for observing and changing an active
Session. It is a separate `netlaglab attach` process and owns no Session
resources. Detach, stdin EOF, Controller protocol failure, or Controller
process failure does not stop the Workload or Session.

Only one Controller may be attached at a time. After detach, a later Controller
may attach to the same active Session.

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

The parser replaces the Controller's direct command-string comparisons in the
first implementation checkpoint. Existing `help`, `status`, `stop`, and
`detach` behavior remains unchanged. A valid Profile Change is recognized but,
until helper dispatch exists, receives
`ERROR Profile changes are not available yet.\n` and leaves the Controller
connected. `help` advertises only commands that can currently complete; it does
not list `set` or `reset` until the runtime backend exists.

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
- Later commands wait while a Profile Change is in flight; `status` cannot
  overtake it.
- Request identifiers are unnecessary in this serialized MVP.
- Controller disconnect does not cancel an already accepted operation.
- A Workload terminal event cancels the pending reply and all queued commands.
  No reply may follow the terminal event.

## Profile and status semantics

Status exposes only the last helper-confirmed current Network Profile. There is
no separate requested or pending profile. While a Profile Change is in flight,
status would still represent the previous confirmed state, but serialization
prevents an attached `status` command from overtaking that Profile Change.

Target status contains:

- Session state: `running` or `stopping`;
- directly managed Workload PID;
- program and arguments, safely escaped and bounded for display;
- outbound and inbound values from the last confirmed Network Profile.

Environment data is never included in status.

## Stop and terminal responses

- An accepted `stop` request produces `STOPPING` and leaves the Controller
  connected for the final result.
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

## Current-to-target gap

The current Supervisor parses all six command families immediately, but it has
no operation queue, helper Profile Change request, confirmed Network Profile
updates, public stopping state, or detailed terminal result. It still emits only
`SESSION_ENDED` and `SESSION_FAILED` without the target result detail.

## Implemented parser checkpoint

The parser checkpoint provides:

- parser tests cover every command type, both directions, every setting,
  `set` and `reset`, whitespace, units, normalization, and every parse error;
- focused regression cases cover 1024/1025-byte lines, an invalid byte,
  overflow, underflow, range rejection, attached-suffix classification, and
  error precedence;
- Controller-conversation tests cover partial framing, fixed error responses,
  continued connections, and disconnect after an oversized command;
- valid `set` and `reset` commands are recognized but truthfully reported as
  unavailable until helper dispatch exists;
- existing `help`, `status`, `stop`, and `detach` behavior remains unchanged;
- `docs/cli.md`, the runtime call map, and the dated MVP audit are updated after
  implementation so their current-state claims remain accurate.

## Open implementation design

- Machine-level response frames for failed Profile Changes.
- Queue representation and integration with helper terminal events.
