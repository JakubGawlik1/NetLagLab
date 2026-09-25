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
- `status` shows the application PID and arguments, followed by the default outbound and inbound
  profiles;
- `stop` requests `SIGTERM`, waits five seconds, then requests `SIGKILL` if the application has
  not exited;
- `detach` disconnects the controller without stopping the application or its supervisor.

The status output says `shaping: not applied`. NetLagLab does not apply the displayed network
profile in this stage of the project.

Only one controller can be connected at a time. After a successful `detach`, another
`netlaglab attach` can connect to the same session. End-of-file on the controller's standard input
(for example, Ctrl-D on an empty terminal line) requests the same controlled detach.

If the application ends while a controller is connected, the controller receives a terminal
session message, prints `Session ended.`, and exits successfully. If the socket instead reaches
EOF without a terminal session message, the result of the application is unknown and the
controller reports:

```text
NetLagLab: connection to session lost; session result is unknown.
```

### Attach exit status

| Status | Meaning |
|---:|---|
| 0 | The controller detached successfully, or the session ended normally. |
| 1 | There is no session, another controller is attached, the connection failed, or the connection ended unexpectedly. |
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
