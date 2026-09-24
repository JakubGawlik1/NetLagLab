# Traffic shaping and Network Profile

## Status

The code contains typed outbound and inbound profile settings plus validation
tests, but the profile is not connected to runtime behavior. Status constructs
an unrestricted default profile and reports `shaping: not applied`. Delay,
jitter, and loss were exercised manually with `tc/netem`; bandwidth limiting
was not established by that experiment.

The semantics and mutation rules below are accepted target design. Runtime
qdisc application and live updates are not implemented.

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
and normalizes them before it creates a typed profile delta; see
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

For every delta:

1. The Supervisor retains the previous confirmed Network Profile and marks one
   operation in flight internally.
2. The helper validates the operation against its state.
3. The helper applies all privileged changes needed for that one setting.
4. Only after complete success does the helper acknowledge the operation.
5. Only after acknowledgement does the Supervisor update its public profile
   and reply successfully to the Controller.

Sequential commands do not make a multi-step `tc` change atomic: one system
operation can succeed and a later one fail. The helper must keep enough private
state to restore the preceding confirmed configuration. If rollback fails or
the actual qdisc state becomes unknown, the Session fails and proceeds to
cleanup.

## Workload exit race

When the helper recognizes Workload exit, it sends the terminal event. An
acknowledgement for an in-flight mutation may already have been sent; if not,
it is no longer required. The terminal event cancels the Supervisor's pending
reply and queued Controller commands, and no operation reply may be emitted
after it.

## Experiment evidence and limits

The manual experiment showed fixed delay, variable delay with jitter, and
packet loss on the two veth endpoints. It also demonstrated that round-trip
ping can conceal direction. It did not prove production rollback, live profile
updates, bandwidth limiting, Controller behavior, helper protocol behavior, or
cleanup.

## Open implementation design

- The exact fixed C++ operations and qdisc command/netlink realization behind
  each typed delta.
- How bandwidth limiting composes with netem while preserving transactional
  replacement and rollback.
- Focused privileged verification for each direction and each supported
  setting, separately authorized from normal unit tests.
