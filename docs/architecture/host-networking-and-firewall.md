# Host networking and firewall

## Status

The production Network Environment configures and owns one scoped IPv4 NAT
table using `nft`. It requires host IPv4 forwarding to be enabled already and
refuses unsupported routing policy, including common split-default VPN routes.
It does not configure host forwarding filters. Forwarded TCP/UDP still depend
on the host firewall allowing the traffic. [Experiment 01](../experiment_01.md)
showed that UFW can allow ICMP while blocking forwarded TCP and UDP. Privileged
TCP/UDP qualification remains separate from the controlled adapter tests.

## Safety boundary

NetLagLab does not:

- enable or disable global `net.ipv4.ip_forward`;
- disable a firewall or weaken its default policy;
- add broad permanent forwarding rules;
- modify or delete rules it does not own;
- assume that a rule in its own nftables table overrides another firewall's
  later filtering decision;
- report successful ping as proof that TCP, UDP, DNS, and application traffic
  work.

The helper may inspect host state, create narrowly scoped Session-owned NAT
objects, and propose an explicitly supported firewall exception. Unsupported,
ambiguous, or unsafe host policy produces an actionable startup refusal.

## Forwarding precondition

IPv4 forwarding must already be enabled by the host administrator. The helper
reads and semantically validates `net.ipv4.ip_forward`; a value of `0` fails
preflight. NetLagLab does not make a global persistent or runtime sysctl change
on the user's behalf.

## NAT ownership

The helper owns a dedicated nftables table named `netlaglab`, tagged with a
random Session ownership comment. The rule matches input from `nll-host` and
source `10.200.0.2`, then applies masquerading as packets leave through the
host's normal route selection. Cleanup verifies the table, chain, and rule all
match the exact Session-owned definition before deleting the table. A
pre-existing table with that name is a collision.

Creating a separate nftables table gives NetLagLab a removable ownership
boundary for its NAT objects. It does not give that table authority over UFW,
firewalld, or another base chain that can still drop forwarded traffic.

## Firewall detection and adapters

Supporting multiple firewall environments means detecting and correctly
handling known cases, not issuing one generic rule everywhere.

The target helper distinguishes:

- active UFW;
- active firewalld;
- no unambiguous supported manager;
- multiple or conflicting active mechanisms.

It mutates only a backend with an explicitly implemented safe adapter.
Multiple, unknown, or conflicting mechanisms cause startup refusal with
diagnostic evidence instead of speculative changes.

### UFW

- Forwarded Session traffic uses a narrowly scoped `ufw route` rule.
- UFW CLI rules are persistent rather than naturally Session-scoped runtime
  objects, so exact ownership, cleanup, and interrupted-run recovery are
  mandatory.
- The rule must name the exact Session source, direction, and interfaces that
  follow from the final route decision.
- NetLagLab removes only the exact rule it recorded as its own.

### firewalld

- NetLagLab may use runtime-only changes when a compatible existing zone or
  policy already exists.
- It does not create a permanent firewalld policy automatically.
- A reload or daemon restart can discard runtime-only changes; the helper must
  treat lost or unknown required state as Session failure rather than continue
  claiming isolation or connectivity.

### No supported manager

A Session-owned nftables table provides scoped NAT. Forwarding filtering must
already permit the traffic; an `accept` verdict in one nftables base chain
cannot be assumed to bypass all other base chains. No firewall adapter is
implemented.

## Two-stage informed consent

Consent is separate from `sudo` authentication.

1. Before invoking `sudo`, the Supervisor explains why root privilege is
   needed and warns that the helper may propose a scoped firewall exception.
2. After authenticated privileged inspection, but before any firewall
   mutation or Workload launch, NetLagLab displays the exact backend, rule
   scope, persistence, and cleanup plan and asks `y/N`.

The exact confirmation is read from `/dev/tty`, never fd 0. Workload standard
input may already be a pipe, and consuming even one byte for a prompt would
permanently remove it from the stream the Workload must inherit.

If a firewall mutation is required but there is no controlling terminal,
automatic mutation is refused with an actionable error. The MVP does not add a
`--yes` mode.

## Transaction and failure behavior

- Record ownership information required for safe cleanup before applying a
  persistent mutation.
- If any network or firewall step fails before Workload launch, roll back every
  resource already created.
- During runtime, loss or ambiguity of required firewall/NAT state is an
  infrastructure failure.
- Cleanup removes only exact Session-owned resources.
- Failure to remove an owned resource causes Session result `125` and feeds
  the recovery path.

The durable recovery mechanism for persistent UFW mutation is not fully
designed. See [Cleanup and recovery](cleanup-and-recovery.md).

## Open decisions

- **Q65:** dynamic host route selection versus a pinned startup uplink. This
  affects the final NAT and firewall rule shape.
- **Q47:** recovery-journal path, schema, write/flush guarantees, state
  transitions, and reconciliation policy.

## Primary references

- [UFW framework and routed forwarding examples](https://manpages.debian.org/unstable/ufw/ufw-framework.8.en.html)
- [firewalld runtime versus permanent configuration](https://firewalld.org/documentation/configuration/runtime-versus-permanent.html)
- [`firewall-cmd` manual](https://firewalld.org/documentation/man-pages/firewall-cmd.html)
- [firewalld policy objects](https://firewalld.org/documentation/man-pages/firewalld.policies.html)
- [nftables manual](https://netfilter.org/projects/nftables/manpage.html)
- [firewalld nftables backend and cross-chain verdict behavior](https://firewalld.org/2018/07/nftables-backend)
