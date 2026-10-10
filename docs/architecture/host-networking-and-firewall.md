# Host networking and firewall

## Status

The production Network Environment creates source-scoped NAT and supports
scoped UFW and firewalld forwarding changes. It refuses unknown, conflicting,
or ambiguous firewall states before Workload activation. Firewall consent and
ownership checks have unprivileged scripted coverage; end-to-end DNS, TCP, and
UDP connectivity remains a separately qualified host-dependent behavior.
[Experiment 01](../experiment_01.md) records the manual evidence that a
successful ping does not prove forwarded TCP or UDP connectivity.

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

The helper owns a dedicated nftables table named `netlaglab_<token>`. Its
masquerade rule matches packets entering through `nll-host` with source
`10.200.0.2/32`. Host routing remains unchanged and is followed dynamically.

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
- The rule names the Session source and ingress interface; it allows the
  Session to follow the host's current egress route.
- NetLagLab removes only the exact rule it recorded as its own.
- A root-owned recovery journal records intent before mutation and is reconciled
  under the host lock before another Session starts.

### firewalld

- NetLagLab may use runtime-only changes when a compatible existing zone or
  policy already exists. Its policy is scoped to the Session source and the
  detected ingress zone.
- It does not create a permanent firewalld policy automatically.
- A reload or daemon restart can discard runtime-only changes; the helper must
  treat lost or unknown required state as Session failure rather than continue
  claiming connectivity.

### No supported manager

A Session-owned nftables table can provide scoped NAT. Whether and how
forwarding filtering may be changed still requires an explicit supported
adapter or a host configuration that already permits the traffic. An `accept`
verdict in one nftables base chain cannot be assumed to bypass all other base
chains.

## Two-stage informed consent

Consent is separate from `sudo` authentication.

1. Before invoking `sudo`, the Supervisor explains why root privilege is
   needed and warns that the helper may request consent for a scoped firewall
   exception.
2. After authenticated privileged inspection, but before any firewall
   mutation or Workload launch, NetLagLab displays the backend, exact rule or
   policy scope, persistence, and cleanup plan and asks `y/N`. UFW's persistent
   rule is removed during cleanup or journal recovery; the firewalld policy is
   runtime-only and removed during cleanup.

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
- While the Workload runs, the helper checks its exact owned NAT table and
  required firewall rule or policy once per second. Missing or mismatched state
  fails the Session, requests orderly Workload termination, and preserves the
  Workload result separately from the connectivity failure.
- Cleanup removes only exact Session-owned resources.
- Failure to remove an owned resource causes Session result `125` and feeds
  the recovery path.

The journal's file format and durability rules are documented in
[Cleanup and recovery](cleanup-and-recovery.md#persistent-ufw-recovery).

## Primary references

- [UFW framework and routed forwarding examples](https://manpages.debian.org/unstable/ufw/ufw-framework.8.en.html)
- [firewalld runtime versus permanent configuration](https://firewalld.org/documentation/configuration/runtime-versus-permanent.html)
- [`firewall-cmd` manual](https://firewalld.org/documentation/man-pages/firewall-cmd.html)
- [firewalld policy objects](https://firewalld.org/documentation/man-pages/firewalld.policies.html)
- [nftables manual](https://netfilter.org/projects/nftables/manpage.html)
- [firewalld nftables backend and cross-chain verdict behavior](https://firewalld.org/2018/07/nftables-backend)
