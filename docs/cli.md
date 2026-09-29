# Command-line interface

## Run a program

```text
netlaglab run -- <program> [arguments...]
```

The `--` separator is required. The first value after it is the program to run, and every
remaining value is passed to that program without interpretation by NetLagLab.

For example:

```bash
netlaglab run -- firefox --private-window
```

The privileged helper resolves a bare program name using the first exact `PATH=` entry in the
captured environment, starts it with the captured argv, environment, working directory, user
identity, supplementary groups, and standard streams, and waits for it to finish. The helper is
the program's direct parent.

This lifecycle checkpoint does not yet create a network namespace or apply shaping. The program
therefore still uses the host network environment.

## Attach a controller

```text
netlaglab attach
```

This command connects a controller in the current terminal to the one active NetLagLab session.
It does not accept arguments. The application continues to use the standard input, output, and
error streams of the terminal in which `netlaglab run` was started.

The controller accepts newline-terminated commands:

- `help` lists the controller commands;
- `status` shows the application PID and arguments, public running/stopping state, and the last
  helper-confirmed outbound and inbound profiles;
- `stop` requests `SIGTERM`, waits five seconds, then requests `SIGKILL` if the application has
  not exited;
- `detach` disconnects the controller without stopping the application or its supervisor.

The Controller validates and dispatches Profile Changes with this syntax:

```text
set <outbound|inbound> <delay|jitter|loss|bandwidth> <value>
reset <outbound|inbound> <delay|jitter|loss|bandwidth>
```

Delay and jitter use milliseconds by default and accept `ms` or `s`. Bandwidth uses `kbps`
by default and accepts `kbps` or `mbps`; it must be greater than zero. Loss uses percent by
default, accepts `%`, and must be between 0 and 100 inclusive. Units may be attached or
separated. Commands and names are lowercase; spaces and tabs may surround and separate tokens.
The production helper currently responds with
`ERROR Profile change could not be applied; previous profile remains active.` because the real
traffic-shaping adapter is not implemented. The request still crosses the typed privilege
boundary and completes deterministically. `help` does not advertise these commands until the
production backend can apply them successfully.

Controller lines are limited to 1024 raw bytes before trimming. At most 32 parsed commands may
wait behind the operation in flight. Ordinary syntax errors return a fixed `ERROR` response and
do not consume queue capacity. An oversized command or queue overflow reports a fixed error and
disconnects the Controller without cancelling an already dispatched Profile Change.

The status output says `shaping: not applied`. NetLagLab does not apply the displayed network
profile in this stage of the project.

Only one controller can be connected at a time. After a successful `detach`, another
`netlaglab attach` can connect to the same session. End-of-file on the controller's standard input
(for example, Ctrl-D on an empty terminal line) requests the same controlled detach.

If the application ends while a controller is connected, the Supervisor waits for helper
cleanup, launcher reaping, and its own cleanup before sending the complete Session Outcome.
For an infrastructure-clean Session, the controller prints either
`Session ended; Workload exit code: <code>.` or
`Session ended; Workload terminated by signal <n>.` and exits successfully. Its exit status
describes successful observation of the Session, so it remains zero even when the Workload exit
code was nonzero.

If Session infrastructure failed, the controller reports every failed infrastructure stage and
then the known Workload result, or explicitly says that the Workload result is unknown. It exits
with status 1. The client still accepts the legacy `SESSION_ENDED` and `SESSION_FAILED` messages
from an older Supervisor without inventing typed details, but a new Supervisor emits only the
structured Session Outcome block.

If the socket instead reaches EOF without a complete terminal Session Outcome, the result of the
application is unknown and the controller reports:

```text
NetLagLab: connection to session lost; session result is unknown.
```

### Attach exit status

| Status | Meaning |
|---:|---|
| 0 | The controller detached successfully, or it observed an infrastructure-clean Session end. |
| 1 | There is no session, another controller is attached, Session infrastructure failed, the protocol was invalid, the connection failed, or the connection ended unexpectedly. |
| 2 | The `attach` command has invalid syntax. |

## Run exit status

| Status | Meaning |
|---:|---|
| 2 | The NetLagLab command has invalid syntax. |
| 125 | Session infrastructure, communication, reaping, or cleanup failed. |
| 126 | The helper could not execute the program because of permission or executable-format failure. |
| 127 | The helper could not find the program. |
| `128 + signal` | The program was terminated by a signal. |

After a program has been started successfully, its normal exit status is returned unchanged.
For example, a program that exits with status 1 makes NetLagLab exit with status 1.

After activation, an infrastructure failure overrides the returned application result with 125,
but the diagnostic preserves the already known application outcome. Statuses 125, 126, and 127
are not reserved application exit values after successful activation, so use the accompanying
diagnostic to distinguish their source.
