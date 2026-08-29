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

NetLagLab searches for the program using `PATH`, starts it with the current environment, and
waits for it to finish. The program inherits the standard input, standard output, and standard
error streams of NetLagLab.

## Exit status

| Status | Meaning |
|---:|---|
| 2 | The NetLagLab command has invalid syntax. |
| 125 | The launcher or `waitpid()` encountered an internal error. |
| 126 | `posix_spawnp()` directly reported that the program could not be executed. |
| 127 | `posix_spawnp()` directly reported that the program was not found. |
| `128 + signal` | The program was terminated by a signal. |

After a program has been started successfully, its normal exit status is returned unchanged.
For example, a program that exits with status 1 makes NetLagLab exit with status 1.

There is a diagnostic limitation around status 127. A successful call to `posix_spawnp()` can
still be followed by a child process exiting with status 127. NetLagLab passes that status
through and cannot reliably determine whether it represents a later failure to execute the
program or an intentional status returned by the program itself.
