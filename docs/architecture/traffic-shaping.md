# Traffic shaping and Network Profile

## Status

The code contains typed outbound and inbound profile settings, validation, and
an integrated Controller-to-helper Profile Change path. The helper applies
delay, jitter, and packet loss through bounded `tc/netem` operations on the
proven Session veth pair. Status owns the last helper-confirmed profile. The
production path has deterministic adapter coverage and passed controlled
privileged directional qualification on 2026-10-11. Bandwidth limiting remains
unsupported.

## Network Profile meaning

A Network Profile is the outbound and inbound network condition most recently
confirmed by the helper as current for a Session.

- There is no separate requested or applied profile.
- A pending operation does not change the public profile.
- A rejected change leaves the previous confirmed profile current if rollback
  succeeds.
- Unknown applied state is not represented as another profile; it fails the
  Session with infrastructure result `125`.

Every Session starts with an unrestricted profile: no delay, jitter, packet
loss, or bandwidth limit. This does not mean skipping namespace, veth,
routing, NAT, DNS, or firewall setup.

The MVP has no initial-profile or configuration-file CLI. Live `set` and
`reset` operations begin only after the Session becomes active.

## Traffic direction

Linux root qdiscs shape egress from an interface. In the accepted topology:

```text
Workload outbound: Workload -> nll-app egress -> nll-host ingress -> host
Workload inbound:  host -> nll-host egress -> nll-app ingress -> Workload
```

Therefore:

- the qdisc on `nll-app` controls Workload outbound conditions;
- the qdisc on `nll-host` controls Workload inbound conditions.

Ping reports round-trip time and crosses both directions, so it cannot prove
which qdisc caused a measured delay. Directional behavior needs asymmetric
settings and protocol-specific observation when verified.

## Settings and validation

Each direction has:

- delay in milliseconds;
- jitter in milliseconds;
- packet loss as a finite percentage from 0 through 100;
- bandwidth in kilobits per second, where zero is invalid when represented as
  a limit.

Delay and jitter cannot be negative. The Controller accepts human-facing units
and normalizes them before it creates a typed Profile Change; see
[Controller control plane](controller-control-plane.md#values-and-normalization).

### Jitter with zero base delay

Jitter greater than zero is allowed when base delay is zero. This follows
direct netem semantics rather than pretending that jitter is a symmetric delay
distribution around zero.

For `delay 0ms 10ms` without an explicit distribution, approximately half of
the raw samples are negative and therefore schedule packets for immediate
dequeue, while the remaining samples add roughly 0–10 ms. NetLagLab does not
currently model an explicit netem distribution. If distributions are added
later, note that the current `tc` parser rejects an explicit distribution when
base delay or jitter is zero.

Primary implementation references:

- [iproute2 netem parser](https://github.com/iproute2/iproute2/blob/main/tc/q_netem.c#L190-L217)
- [iproute2 netem option construction](https://github.com/iproute2/iproute2/blob/main/tc/q_netem.c#L517-L554)
- [Linux netem delay sampling](https://github.com/torvalds/linux/blob/master/net/sched/sch_netem.c#L535-L588)
- [Linux netem enqueue/dequeue behavior](https://github.com/torvalds/linux/blob/master/net/sched/sch_netem.c#L682-L749)

## Live profile operations

Controller `set` and `reset` commands become typed, per-setting deltas. The
Supervisor sends one delta at a time through the helper adapter instead of
sending CLI text or replacing a full profile.

`ProfileChange` is a shared domain type beside `NetworkProfile`, rather than a
Controller-owned type or a separate wire DTO. One pure domain operation applies
a Profile Change to a supplied Network Profile and returns the resulting
profile. The operation performs no I/O and does not decide whether a change is
confirmed. The Supervisor calls it only after `PROFILE_OK`; the prepared
Network Environment uses it to track and restore its preceding confirmed
profile after failure.

The operation validates the complete resulting profile and returns
`std::variant<NetworkProfile, std::vector<ValidationError>>`. The error
alternative is never empty. It never partially mutates its input and does not
assume that every caller obtained the Profile Change from the Controller
parser. Resetting delay, jitter, or loss produces zero; resetting bandwidth
produces no limit. Parser and helper-boundary validation remain in place even
though the domain operation also rejects an invalid constructed value.

For every delta:

1. The Supervisor retains the previous confirmed Network Profile and marks one
   operation in flight internally.
2. The helper validates the operation against its state.
3. The helper validates the exact retained namespace and both veth identities,
   then applies one complete directional qdisc with a trusted fixed-path `tc`.
4. Only after complete success does the helper acknowledge the operation.
5. Only after acknowledgement does the Supervisor update its public profile
   and reply successfully to the Controller.

Sequential commands do not make a multi-step `tc` change atomic: one system
operation can succeed and a later one fail. The helper must keep enough private
state to restore the preceding confirmed configuration. Attempt and rollback
each have a separate bounded command deadline. If rollback fails or the
interface identity can no longer be proven, the Session fails and proceeds to
cleanup. The qdiscs are attached to the Session-owned veth links and are removed
with those links during Network Environment cleanup.

## Workload exit race

When the helper recognizes Workload exit, it sends the terminal event. An
acknowledgement for an in-flight Profile Change may already have been sent; if
not, it is no longer required. The terminal event cancels the Supervisor's
pending reply and queued Controller commands, and no operation reply may be
emitted after it.

## Experiment evidence and limits

The manual experiment showed fixed delay, variable delay with jitter, and
packet loss on the two veth endpoints. It also demonstrated that round-trip
ping can conceal direction. It did not prove production rollback, live profile
updates, bandwidth limiting, Controller behavior, helper protocol behavior, or
cleanup.

## Production qualification (2026-10-11)

The user ran the opt-in
`TrafficShapingQualification.MeasuresDirectionalDelayJitterLossAndReset` case
against the production Session, helper, and Controller. It passed in 30,558 ms
with `qualification_exit=0`. The case required a real local UDP handshake and
baseline, then confirmed outbound delay of 80 ms, inbound delay of 120 ms,
outbound jitter of 30 ms with zero base delay, 40% loss independently in both
directions, and live per-setting resets. Timing spread and loss use statistical
bounds rather than exact sample values. Session stop, Workload reaping, and
topology/lock cleanup passed; the subsequent inventory also showed no recovery
journal, token-matched UFW rule, owned nftables table, namespace, or host veth.

The host kept UFW active with incoming and routed traffic denied by default.
The user approved the exact source-scoped forwarding rule, which cleanup
removed. The diagnostic wrapper supplied a supported IPv4 resolver snapshot in
a private mount namespace; it did not change the host resolver configuration.
The nonprivileged CTest selection passed 262/262 tests, and the implementation
and integration corrections received separate standards and spec reviews.

- The controlled local IPv4/UDP qualification receives outbound traffic at
  `nll-host` through a filtered packet socket after `nll-app` egress. Inbound
  traffic is sent through an ordinary host UDP socket and measured at the
  Workload after `nll-host` egress. Both use the host monotonic clock for
  independent one-way measurements. Host INPUT policy cannot prevent outbound
  observation; no additional firewall exception is added for the measurement.
  This passing case does not qualify host UDP socket access or Internet DNS,
  TCP, and UDP, which have separate checks.

## Remaining qualification

- Other supported resolver/firewall configurations and the separate Internet
  DNS, TCP, and UDP case remain unqualified by this run.
- Bandwidth limiting and its composition with netem remain out of scope.
