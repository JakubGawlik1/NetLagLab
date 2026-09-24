# Controller control plane

## Status

The current Controller supports `help`, `status`, and `detach` over
`control.sock`. Status always shows an unrestricted default profile and
`shaping: not applied`. The accepted target adds typed profile mutations and
Session stop while preserving the existing one-Controller, line-oriented
model.

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
instead of rounding.

The Supervisor normalizes values at the Controller parser seam before
creating a typed profile delta. Examples include:

```text
set outbound delay 1 s
set inbound jitter 250ms
set outbound loss 1.5%
set inbound bandwidth 5 mbps
```

## Sequencing

- The Supervisor processes at most one Controller operation at a time.
- `set` and `reset` receive success only after helper confirmation.
- Later commands wait while a mutation is in flight; `status` cannot overtake
  it.
- Request identifiers are unnecessary in this serialized MVP.
- Controller disconnect does not cancel an already accepted operation.
- A Workload terminal event cancels the pending reply and all queued commands.
  No reply may follow the terminal event.

## Profile and status semantics

Status exposes only the last helper-confirmed current Network Profile. There is
no separate requested or pending profile. While a mutation is in flight, status
would still represent the previous confirmed state, but serialization prevents
an attached `status` command from overtaking that mutation.

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

The current Supervisor handles Controller commands immediately and recognizes
only `help`, `status`, and `detach`. It has no typed parser for settings, no
operation queue, no helper mutation request, no stopping state, and only the
older terminal messages `SESSION_ENDED` and `SESSION_FAILED` without the target
result detail.

## Open implementation design

- Exact user-facing error text and machine-level response frames for invalid
  values and failed mutations.
- Queue representation and integration with helper terminal events.
- Focused parser tests for whitespace, attached/separated units, exact
  conversion, overflow, and extra tokens.
