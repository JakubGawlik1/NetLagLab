# Traffic shaping and Network Profile

## Status

The code contains typed outbound and inbound profile settings, validation, and
an integrated Controller-to-helper Profile Change path. The helper applies
delay, jitter, and packet loss through bounded `tc/netem` operations on the
proven Session veth pair. Status owns the last helper-confirmed profile. The
production path has deterministic adapter coverage; real privileged
directional qualification has not been run for this change. Bandwidth limiting
remains unsupported.

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

## Remaining qualification

- Run focused privileged verification for each direction and supported
  setting, separately authorized from ordinary automated tests. Use asymmetric
  controlled traffic so delay, jitter, and loss evidence identifies the
  direction being exercised.
- Bandwidth limiting and its composition with netem remain out of scope.
