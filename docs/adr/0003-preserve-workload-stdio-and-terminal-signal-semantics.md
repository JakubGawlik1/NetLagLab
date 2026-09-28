---
status: accepted
---

# Preserve Workload streams without promising transparent terminal job control

Non-terminal standard streams retain their original pipe, file, socket, and
redirection semantics across the privilege boundary. A terminal stream may use
the pseudo-terminal created by `sudo`; the MVP preserves basic interactive I/O
and terminal detection but does not promise the original `/dev/tty`, process
group, or full job-control behavior. A first terminal Ctrl-C reaches the
Workload exactly once while the Supervisor only observes it and starts the
accepted grace period. The helper survives its copy of that signal, and a
second Ctrl-C requests immediate `SIGKILL`. We chose this explicit limitation
over requiring host `sudoers` changes or claiming terminal transparency the
process topology cannot guarantee.
