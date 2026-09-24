# Workload execution

## Status

The current Supervisor starts the Workload directly with `posix_spawnp()`.
The accepted target makes the helper the Workload's direct parent so it can
enter the child into the Session namespaces before permanently dropping to the
invoking user's identity. The target is not implemented.

## Ownership boundary

NetLagLab owns, stops, and reaps only the directly managed Workload process.
Descendants may inherit its network environment, but NetLagLab does not own,
track, signal, or reap them, and they may outlive the Session.

The helper owns the directly managed Workload process in the target design.
The long-lived helper remains privileged and outside the Workload's network
namespace so it can manage host-side resources, apply profile changes, receive
Supervisor requests, signal the Workload, reap it, and clean up.

## Current implementation

After receiving helper `READY`, the Supervisor calls:

```text
posix_spawnp(child_arguments[0], child_arguments, environ)
```

The Workload is therefore a direct child of the Supervisor, stays in the
host's namespaces, inherits the current environment and standard descriptors,
and is reaped by the Supervisor. No namespace, veth, DNS mount, routing, NAT,
or shaping setup affects it.

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
and standard input/output/error. Dropping UID alone is not considered enough
to restore the user's execution context.

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
- The standard descriptors are preserved for the Workload and are not reused
  for consent input.
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
