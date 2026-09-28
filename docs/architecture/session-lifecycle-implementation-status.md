# Session lifecycle implementation status

## Checkpoint

The 2026-09-25 checkpoint implements the Supervisor/helper/Workload lifecycle
and its deterministic test seams without claiming that the network environment
exists. The accepted target contract remains in
[Session lifecycle](session-lifecycle.md); the exact current call graph is in
the [runtime call map](../runtime_call_map.md).

## Implemented

- one blocking Supervisor lifecycle seam returning a structured
  `SessionOutcome` only after helper cleanup and launcher reaping;
- lossless bounded argv/environment/cwd transport and a 30-second START-block
  deadline, plus explicit non-terminal standard-descriptor transfer with
  terminal/closed-descriptor preservation;
- mutual `SO_PEERCRED` checks, launcher-process-tree binding, a root-owned
  `/run/netlaglab/host.lock`, and publication of `control.sock` only after
  explicit activation; while waiting for the initial connection, the helper
  also watches a `pidfd` bound to the launching Supervisor and verifies its
  `/proc` start time to reject PID reuse before `pidfd_open()`;
- helper-owned `fork`/`execve`, exact UID/GID/supplementary-group restoration,
  `PR_SET_PDEATHSIG(SIGKILL)`, `PR_SET_NO_NEW_PRIVS`, direct-PID signalling,
  Workload reaping, and exec-success evidence through a close-on-exec pipe;
- legal helper-conversation ordering, partial/coalesced stream framing,
  activation/start-failure/Workload-result/cleanup events, and terminal EOF
  handling;
- non-blocking self-pipe signal integration, Controller `stop`, first/second
  Ctrl-C escalation, terminal-event cancellation of queued Controller/signal
  events, helper-loss handling, and bounded launcher reaping;
- focused lifecycle, protocol, context, and local-process tests.

## Not implemented in this checkpoint

The following work is intentionally absent rather than partially simulated:

- network and mount namespaces;
- `nll-host`/`nll-app`, addresses, routes, forwarding preflight, and NAT;
- Session DNS view;
- UFW/firewalld adapters, mutation consent, and persistent recovery;
- qdisc/netem application, bandwidth composition, live profile mutations, and
  helper acknowledgements for profile changes;
- interrupted privileged-resource reconciliation.

Therefore `netlaglab run` still gives the Workload the host network environment
and `status` continues to report `shaping: not applied`.

## Decisions required before the network backend

1. **Q64 — DNS contents:** resolver snapshot or host-side proxy.
2. **Q65 — host route:** dynamic host routing or one pinned startup uplink.
3. **Q47 — persistent recovery:** journal path, schema, durability,
   transitions, and reconciliation policy.

The backend must not invent these choices. A coherent next implementation
slice can build namespace/veth/local routing that does not require Internet or
DNS, but production Internet connectivity, firewall mutation, and durable
recovery remain blocked by the decisions above.

## Verification still requiring explicit authorization

- privileged end-to-end startup on the host through the actual `sudo` policy;
- creation and permissions of `/run/netlaglab/host.lock` as root;
- exact terminal and redirected-pipe behavior across the real
  Supervisor -> `sudo` -> helper -> Workload topology;
- destructive signal escalation against a controlled long-running probe;
- all future namespace, firewall, NAT, DNS, and qdisc integration checks.

The current automated suite is unprivileged. It proves the pure lifecycle and
conversation contracts plus local process behavior, not privileged host
integration.
