# Workload execution

## Status

The helper now forks, launches, signals, and reaps the directly managed
Workload. The child enters the exact retained Session network namespace before
creating a private mount namespace and installing its read-only DNS/NSS view.
It then restores the invoking identity and transmitted execution context.

## Ownership boundary

NetLagLab owns, stops, and reaps only the directly managed Workload process.
Descendants may inherit its network environment, but NetLagLab does not own,
track, signal, or reap them, and they may outlive the Session.

The helper owns the directly managed Workload process.
The long-lived helper remains privileged and outside the Workload's network
namespace so it can manage host-side resources, apply profile changes, receive
Supervisor requests, signal the Workload, reap it, and clean up.

## Current implementation

After `READY`, the Supervisor transfers standard descriptors and the bounded
start block. The helper then performs:

```text
fork
  -> establish PR_SET_PDEATHSIG
  -> enter the exact retained Session network namespace
  -> create a private mount namespace with private propagation
  -> bind sealed resolver and NSS snapshots read-only at /etc/resolv.conf and /etc/nsswitch.conf
  -> map standard descriptors
  -> restore supplementary groups, GID, and UID
  -> set PR_SET_NO_NEW_PRIVS
  -> chdir to transmitted cwd
  -> execve with transmitted argv and environment
```

An exec-status pipe distinguishes successful execution from `125`, `126`, or
`127` start failure. The helper snapshots bounded, supported host IPv4 DNS
configuration before forking and creates immutable in-memory backing files.
The child mounts those files only in its private mount namespace before
dropping privileges. Host resolver files and the helper's mount namespace are
unchanged. Unsupported resolver policy or a mount failure prevents activation;
the helper reports a diagnostic and cleans the prepared Network Environment.
The Workload still has no Internet NAT or traffic shaping.

## Accepted target launch sequence

The helper creates a child and that child performs the privileged-to-user
transition in this order:

```text
fork in the helper
  -> enter the Session network namespace
  -> establish the private mount view required for Session DNS
  -> set supplementary groups, GID, and UID for the invoking user
  -> drop remaining capabilities
  -> change to the transmitted working directory
  -> install the transmitted environment
  -> exec the original program without a shell
```

Namespace and mount operations that require privilege happen before the
identity and capability drop. The Workload must execute with the invoking
user's UID, GID, supplementary groups, environment, current working directory,
and standard input/output/error. Non-terminal descriptors preserve their pipe,
file, socket, and redirection semantics. Terminal descriptors may use the
pseudo-terminal created by `sudo`; basic interactive I/O and terminal detection
remain supported, but the MVP does not promise the original `/dev/tty`, process
group, or full job-control behavior. Dropping UID alone is not considered
enough to restore the user's execution context.

Linux cannot move an arbitrary already-running process into a different
network namespace. `setns()` changes the calling thread, while `ip netns
attach` only gives a name to the namespace a process already occupies. This is
why the helper must create the child and place it before `exec`.

## Execution-context transport

The Supervisor sends a one-time full execution-context snapshot through the
authenticated `helper.sock`; it does not rely on `sudo -E` or helper command
line arguments for user data.

- `argv` is preserved exactly, including the original `argv[0]`.
- The environment is the full ordered snapshot inherited by `netlaglab`.
  Duplicate entries and their order are preserved.
- The current working directory is sent as an absolute path.
- Non-terminal standard descriptors are transferred as descriptors so their
  underlying open-file semantics survive the `sudo` boundary. Terminal streams
  may instead use the `sudo` pseudo-terminal and are subject to the documented
  job-control limitation. Standard descriptors are never reused for consent
  input.
- The root helper treats environment data as inert child data: it does not put
  it in the root process environment, interpret it there, or log it.

The start-block encoding, bounds, and failure behavior are specified in
[Supervisor-helper protocol](supervisor-helper-protocol.md).

## Program lookup

- An `argv[0]` containing `/` is executed as the transmitted path. Relative
  paths resolve after changing to the transmitted working directory; absolute
  paths remain direct.
- A bare program name is resolved using the first exact `PATH=` entry in the
  transmitted environment.
- NetLagLab does not invent a fallback `PATH` and does not pre-resolve the
  Workload executable in the Supervisor.
- Missing `PATH` for a bare name, failed lookup, permission denial, or invalid
  executable format is a pre-activation start failure.

If the working directory is renamed or deleted before the child calls
`chdir()`, startup fails safely. Passing a directory descriptor with
`SCM_RIGHTS` is intentionally outside the MVP.

## Terminal and parent failure behavior

- A first terminal Ctrl-C reaches the Workload exactly once. The Supervisor
  observes it to start the grace period but does not forward another `SIGINT`.
- The helper survives its own copy of terminal `SIGINT`; the Workload does not
  inherit that helper-side handling policy.
- `PR_SET_PDEATHSIG(SIGKILL)` protects the directly managed Workload if its
  helper parent dies unexpectedly.
- Controller `stop` and Supervisor loss use the target stop sequence described
  in [Session lifecycle](session-lifecycle.md).
- NetLagLab signals only the directly managed Workload PID. It does not claim
  to control a descendant process group.
- The helper reaps the directly managed Workload and reports its terminal
  result before privileged cleanup completes.

## Descendants and namespace lifetime

An unmanaged descendant may keep an anonymous network namespace alive after
NetLagLab removes the namespace name and veth. This is compatible with the
ownership model and is not a leak of a NetLagLab-owned resource. NetLagLab does
not claim that successful named-namespace deletion proves destruction of the
underlying kernel namespace object.
