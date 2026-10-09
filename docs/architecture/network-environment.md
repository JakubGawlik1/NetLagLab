# Network environment

## Status

The active NetLagLab Session prepares the fixed host-local topology through the
private Network Environment library. The library owns the root host lock,
namespace and veth proofs, setup transaction, and explicit cleanup. The helper
passes a narrow capability for entering the exact retained namespace to the
forked Workload child; the long-lived helper remains in the host network
namespace. The production Session path has deterministic coverage but has not
yet passed the separate privileged qualification. DNS contents and host route
selection remain deliberately open.

The private transaction foundation is implemented and tested without
privilege. One preparation operation drives the complete fixed semantic setup
sequence through a uniquely owned scripted adapter and monotonic clock. It
returns a move-only prepared owner or a typed failure that independently
preserves the primary failure, ordered rollback failures, and any opaque
residual cleanup owner. Explicit cleanup and residual retry are consuming,
dependency-aware, and bounded; destruction makes one no-throw best-effort pass.
A shared trace keeps semantic calls and absolute deadlines observable after the
dependencies move into an owner.

The production entry point now acquires the root-owned host lock before its
read-only preflight. That preflight validates root execution and a root-owned,
non-writable fixed-path `ip`; checks the fixed namespace path and both host link
names; inventories host IPv4 address prefixes; and performs a bounded,
validated `RTM_GETROUTE` dump across all routing tables. Controlled tests cover
exact, broader, and narrower overlaps, permitted default routes, multiple
tables, multipart completion, netlink errors, truncation, malformed attributes,
typed failures, and the absence of later mutation after rejection.

The same private library contains a bounded command runner that executes an
exact path with separate arguments and an empty environment, enforces an
absolute deadline, captures at most 4 KiB of standard error while draining the
remainder, and terminates and reaps a timed-out child. Its production adapter
creates `netlaglab`, retains and identifies the exact `nsfs` object, creates the
veth pair, and moves `nll-app` through the inherited namespace-file descriptor.
Validated `RTM_GETLINK` replies prove both endpoints before and after placement;
namespace-side queries run in bounded short-lived children that enter the exact
handle. Ambiguous mutations are reconciled before ownership is recorded, and
cleanup verifies the retained identities before deleting veth then namespace.
An identity mismatch or unavailable proof is retained without name-based
deletion authority.

Controlled tests cover exact arguments, runner-result mapping, ownership
transitions, exact-handle reuse, ambiguous effects, cleanup ordering, mismatch
refusal, and the post-namespace-removal veth recheck. After proving the roots,
the adapter assigns `10.200.0.1/30`, brings up `nll-host`, enters the exact
namespace handle to bring up loopback, assigns `10.200.0.2/30`, brings up
`nll-app`, and adds the default route through `10.200.0.1`. Each mutation is
one bounded child operation; no redundant readiness readback is added.
Preparation returns a prepared owner only after that complete sequence. The
helper explicitly consumes the prepared or residual owner after Workload
reaping. A cleanup failure remains an infrastructure failure even if the
bounded residual pass later removes the remaining resources.

## Target topology

```text
host network namespace                    Session network namespace: netlaglab

nll-host                                  nll-app
10.200.0.1/30  <------ veth pair ------>  10.200.0.2/30
                                             |
                                             +-- Workload
```

The fixed MVP resources are:

- named network namespace `netlaglab`;
- host veth endpoint `nll-host`;
- Session veth endpoint `nll-app`;
- IPv4 subnet `10.200.0.0/30`;
- host address `10.200.0.1`;
- Workload-side address `10.200.0.2`;
- nftables table `netlaglab` for Session-owned NAT resources.

The MVP is IPv4-only. Multiple Sessions, dynamic subnet allocation, IPv6, and
automatic renaming are outside the current scope.

## Ownership and collision policy

The helper owns every namespace name, veth, address, route, qdisc, mount, DNS
snapshot, NAT object, and approved firewall exception that it creates for the
Session.

Preflight rejects any fixed-name collision or overlapping address/route. The
helper never adopts, modifies, or deletes a pre-existing object merely because
its name matches `netlaglab` or begins with `nll`. Only a future verified
recovery-journal record may establish ownership of stale resources from an
interrupted earlier Session.

For the fixed `10.200.0.0/30` subnet, overlap means any IPv4 address prefix in
the host network namespace that intersects the subnet, or any intersecting
non-default IPv4 route in any host routing table. This includes a broader route
such as a VPN-owned `10.0.0.0/8` and a narrower route such as a host `/32`;
ordinary `/0` default routes remain valid. Addresses and routes in unrelated
network namespaces are outside this collision check. The MVP refuses a
collision rather than selecting a different subnet or overriding host policy.

The helper holds the root-owned global host lock throughout preflight, setup,
runtime, and cleanup so two users cannot race over these host-wide fixed
resources.

## Setup and readiness

The helper performs the setup as a transaction. The required mechanisms are:

1. Validate fixed names, subnet availability, required tools/kernel support,
   and semantic host prerequisites.
2. Create the named network namespace and veth pair.
3. Move `nll-app` into the Session namespace.
4. Assign addresses and bring up `nll-host`, `nll-app`, and namespace loopback.
5. Add the namespace default route through `10.200.0.1`.
6. Prepare the private mount view used to expose a Session-specific read-only
   `/etc/resolv.conf` to the Workload.
7. Install Session-owned NAT and any explicitly approved firewall exception.
8. Apply the unrestricted initial Network Profile.
9. Launch the Workload inside the network and mount environment.

After creating the named network namespace, the helper opens it and retains a
descriptor for its exact kernel object. Moving `nll-app` targets that namespace
file rather than resolving the name again. Each namespace-side iproute2
operation runs in a short-lived child that calls `setns()` on the descriptor
before `execve()`; the long-lived helper remains in the host network namespace.

Each precondition is checked semantically, but read-only preflight promises only
what it can observe without a trial mutation. It acquires the global host lock,
validates the privileged process and trusted tool, rejects fixed-name and subnet
collisions, and proves that the required inventory can be read. It does not
claim in advance that the kernel or current capabilities will permit namespace
creation, veth creation, or `setns()`; failure of one of those mechanisms is a
`system failure` at the first corresponding mutation and follows normal
rollback. It also does not require `net.ipv4.ip_forward` to be enabled: that
prerequisite belongs to later host-routing, NAT, and firewall integration.
Namespace paths, host link names, and address prefixes use narrow system query
interfaces; routes in all host tables use a validated read-only rtnetlink dump.
Mutating iproute2 commands are not used as substitutes for collision preflight.

Positive completion of every setup operation is accepted as readiness. The
MVP does not add redundant full-state readback, public ping, public DNS lookup,
or Internet-connectivity tests. Those tests would impose external policy and
availability as startup requirements without proving every relevant protocol.

Each iproute2 child operation has a five-second deadline. The setup transaction
has one non-renewing 30-second budget, and rollback receives a separate
30-second budget so setup delay cannot consume cleanup time. The coordinator
passes an absolute operation deadline bounded by both the transaction deadline
and the five-second child limit; an adapter cannot renew either budget. A
timed-out child is terminated and reaped. The adapter returns a typed cause and
captures at most 4 KiB of diagnostic standard error, while the coordinator
assigns the semantic stage. The helper presents stable diagnostics rather than
treating raw tool text as a protocol contract.

Deadlines bound starting new work and the normal execution wait. Once a child
has started, timeout termination and reaping must finish even if kernel
finalization extends beyond the nominal deadline. During rollback, an earlier
semantic failure does not stop cleanup of an independent proven resource while
budget remains. Once the cleanup budget expires, the coordinator starts no new
mutations and retains every unattempted or unresolved target in the residual
cleanup owner.

Any failed setup step rolls back all resources already created before a
Workload is launched.

## Transaction module boundary

The helper prepares the local Network Environment through one operation. It
does not drive public preflight, creation, configuration, commit, or rollback
phases. Success returns a move-only prepared environment that owns the ledger,
the exact network-namespace handle needed by a future Workload launch, and an
explicit cleanup operation. Failure identifies the setup stage and returns an
opaque residual cleanup owner only when rollback could not prove that all
created resources were removed. The prepared handle exposes no mutable ledger
or individual setup steps.

The prepared environment and residual cleanup owner also own the capability
needed to execute their cleanup. They do not borrow an adapter whose lifetime a
caller must preserve. The exact namespace handle is move-only and opaque: the
transaction coordinator can retain it and pass it back to semantic operations,
but cannot obtain its descriptor, namespace path, or a raw `setns()` operation.

The production entry point acquires the root-owned global host lock before
preflight. A private move-only runtime containing that lock, the semantic
adapter, and the monotonic clock then moves into the prepared or residual owner
and remains there through cleanup, retry, and any destructor safety pass. The
module releases the lock only after the owner no longer has cleanup work. The
helper no longer acquires a separate lock: it relies on the transaction-owned
lock for preparation, Workload execution, and explicit cleanup.

The public module surface consists of concrete move-only owner types with opaque
implementations and consuming prepare, cleanup, and residual-retry operations.
The ledger, proof types, port, clock, host lock, and exact namespace descriptor
remain private. Tests inject a uniquely owned scripted adapter and clock; a
separate shared trace keeps calls and deadlines observable after those
dependencies have moved into an owner.

The coordinator uses an internal port of fixed, named semantic operations, not
a generic command bus or an externally configurable network builder. Its
operations are preflight, namespace creation, veth-pair creation, peer
placement, each individual address/link/loopback/route mutation, and removal of
the two independent resource roots. The fixed MVP names, addresses, and route
are private to the module. The production Linux/iproute2 adapter and a scripted
test adapter implement this same port; process arguments, child management,
rtnetlink framing, and filesystem mechanics remain behind it.

Resource-creating and peer-placement operations return opaque, move-only
ownership proofs. The types distinguish the exact namespace, a veth pair whose
endpoints are both still on the host, and a pair whose Session endpoint was
verified in the exact namespace. Address, link-state, loopback, and route
operations create no new ownership proof; positive completion records only a
configuration milestone on an already proven resource. They do not require
redundant full-state readback.

Veth ownership is established with read-only `RTM_GETLINK` queries rather than
iproute2 presentation output or an additional `ethtool` executable. Before the
move, both endpoints are queried in the host namespace. After the move, the host
endpoint is queried on the host and the Session endpoint in a short-lived child
that enters the exact namespace handle. The proof requires the expected names
and interface indices, veth link kind, and mutually consistent peer and
namespace information. Because cross-namespace peer reporting is the riskiest
kernel assumption in this design, a focused privileged experiment must confirm
the exact attributes on a supported host before the production proof decoder is
treated as qualified.

An exit, signal, or timeout does not by itself establish whether a create,
peer-placement, or removal mutation took effect. The adapter reconciles the
semantic postcondition and reports the proven state before the operation, the
proven state after it, proven absence, or an identity-unconfirmed residual. A
name found after a create attempt is not adopted as owned without the required
identity evidence. There is no cleanup operation accepting the unconfirmed
type, so later code cannot turn a matching name into deletion authority.

The local transaction checkpoint performs one semantic mutation per iproute2
child, except that creating a veth pair is one kernel operation:

1. complete all preflight checks without mutation;
2. create `netlaglab`, open its descriptor, and record its identity;
3. create `nll-host`/`nll-app` and record the peer identities;
4. move `nll-app` through the namespace file and record its target identity;
5. assign `10.200.0.1/30` to `nll-host`;
6. bring up `nll-host`;
7. bring up `lo` in the exact namespace;
8. assign `10.200.0.2/30` to `nll-app` in that namespace;
9. bring up `nll-app` in that namespace;
10. add the default route through `10.200.0.1`; and
11. return the prepared environment.

The adapter does not batch several independently failing mutations into one
command. A completed child operation therefore corresponds to one semantic
operation and one ledger transition; several operations may map to the same
stable public stage.

Preparation and cleanup failures are typed on two independent axes. The stage
is `preflight`, `namespace`, `veth`, `host configuration`, `namespace
configuration`, `route`, or `cleanup`. The cause is `collision`, `unavailable
or invalid tool`, `system failure`, `command exit`, `command signal`, `timeout`,
`identity unavailable`, `identity mismatch`, or `incomplete cleanup`. `System
failure` covers failures of direct mechanisms such as process creation, pipes,
namespace handles, or inventory reads that would be misclassified as a tool or
identity error. `Identity unavailable` means that an otherwise successful query
could not produce complete and consistent ownership evidence. Presentation maps
these values to stable diagnostics; bounded raw tool output and `errno` remain
local diagnostic context rather than protocol or control-flow input.

A preparation failure preserves three facts independently: the primary setup
failure, every rollback failure in attempted order, and an optional residual
cleanup owner containing only unresolved state. `Incomplete cleanup` is the
infrastructure summary of that result; it does not replace the original setup
stage and cause.

## Verification ownership

Each contract has one primary test layer rather than being repeated through a
full cross-product of implementations:

- pure transaction-coordinator tests use a scripted semantic adapter for setup
  order, ledger transitions, rollback, deadlines, and residual ownership;
- production Linux/iproute2 adapter tests use controlled runner and inventory
  seams for fixed arguments, identity capture, compensation, reconciliation,
  and cleanup authorization;
- command-runner tests execute a small dedicated probe for real process exit,
  signal, timeout, termination, reaping, argument and environment boundaries,
  and bounded diagnostic output, without requiring `setns()`;
- pure inventory, prefix-overlap, and rtnetlink-decoder tests use controlled
  byte fixtures and never depend on the current host routing table; and
- the opt-in privileged smoke test alone verifies the real composition of the
  kernel, iproute2, exact namespace handle, veth, addresses, route, local packet,
  and cleanup.

The coordinator suite does not inspect argv or netlink bytes. Adapter tests do
not repeat every rollback prefix. Runner tests do not know Network Environment
stages, and the privileged smoke does not repeat the deterministic fault matrix.

The coordinator matrix uses the operation sequence `N`, `V`, `M`, `HA`, `HU`,
`LO`, `NA`, `NU`, and `R` for namespace creation, veth creation, peer placement,
host address, host link-up, loopback link-up, namespace address, namespace link-
up, and route. It covers:

- the complete preparation path and explicit cleanup;
- one parameterized failure at every operation, with the exact public stage,
  no later setup call, and rollback of only the proven ledger prefix;
- the distinct old-state, new-state, absent, and unconfirmed postconditions for
  `N`, `V`, and `M` where ownership evidence can change;
- removed, already-absent, identity-mismatch, and failed cleanup outcomes for
  each root, including both roots failing and ordered aggregation;
- residual retry touching only unresolved state and never deleting an
  identity-unconfirmed residual;
- setup deadline expiry before and between operations, the independent rollback
  budget, cleanup expiry, and a fresh budget for explicit residual retry; and
- every stable cause in one representative operation rather than at every step.

For `HA`, `HU`, `LO`, `NA`, `NU`, and `R`, a failure before the mutation and an
ambiguous failure after it are equivalent at the coordinator seam: both retain
the same ownership proofs and roll back the same roots. The production adapter,
not the coordinator matrix, tests the underlying mechanical distinction.

Production-adapter tests verify one fixed runner request per semantic mutation,
the veth pair as one request, exact argument boundaries, reuse of the exact
namespace handle, preflight through inventory rather than trial mutation, and
each ownership transition and compensation outcome for `N`, `V`, and `M`.
Cleanup tests cover matching identity, absence, mismatch, query failure, an
ambiguous delete reconciled to absence or retained identity, and the veth
recheck after namespace removal. Each runner result kind needs one adapter
mapping test, not one test per operation.

Command-runner tests use a compiled probe and cover zero and nonzero exit,
signal termination, an already-expired deadline, timeout plus reaping, exec
failure, exact argv and minimal environment, bounded stderr below and above the
4 KiB capture limit, and draining excess stderr without deadlock. Pure inventory
and decoder fixtures cover exact, broader, and narrower prefix overlap, ignored
default routes, multiple routing tables, multipart completion and error, and
truncated or malformed netlink messages. Default tests do not assert on the
current host's live routing table.

## Implementation packaging

The standalone checkpoint builds a private static
`netlaglab_network_environment` library. Its module interface and all internal
headers remain under `src/`; nothing is added to the public `include/netlaglab/`
domain surface. The library contains the transaction coordinator and owner
implementations, Linux/iproute2 adapter, host lock, command runner, typed
inventory, and pure rtnetlink link/route decoders. It has no dependency on
`netlaglab_core`, helper protocol, Workload execution, DNS, NAT, firewall, or
traffic shaping, and this checkpoint does not link it into `netlaglab-helper`.

Default verification uses one `netlaglab_network_environment_tests` executable
with separate test files for each seam and one
`netlaglab-network-command-probe` executable. The privileged smoke is compiled
and registered only when a dedicated CMake option, off by default, is enabled;
it is labelled `privileged`, runs serially, refuses insufficient privilege, and
never invokes `sudo` itself.

## Standalone checkpoint acceptance

The implementation-ready contract for this checkpoint was confirmed on
2026-09-30. Acceptance requires all of the following:

1. The private standalone library builds without being linked or called by the
   helper, and current Session behavior remains unchanged.
2. Preparation, explicit cleanup, residual retry, and destructor fallback obey
   the accepted ownership, failure-preservation, deadline, and host-lock
   contracts without granting name-based cleanup authority.
3. The production adapter uses fixed semantic operations, trusted separate
   arguments, the exact namespace handle, typed inventory, and reconciled
   ownership proofs; raw diagnostics never control behavior.
4. The default unprivileged tests pass the complete reduced coordinator matrix
   and the focused adapter, runner, inventory, and decoder contracts without
   depending on live host routes or privileged state.
5. A separately authorized experiment confirms the required cross-namespace
   `RTM_GETLINK` veth attributes, and the opt-in privileged smoke then verifies
   the topology, local UDP packet, explicit cleanup, and absence of owned roots.
6. The affected targets build with the configured warnings, the full default
   CTest suite passes after the CMake change, and `git diff --check` is clean.

This section records the acceptance criteria for the standalone checkpoint;
it is not a statement of the current implementation boundary. The helper now
uses the transaction for Session preparation and cleanup, and the Workload
enters the retained namespace in its forked child. Privileged qualification of
that integrated Session path remains separate from the standalone adapter
qualification described here.

## Focused privileged qualification

After this module and its production adapter are implemented, a separately
authorized smoke test must create the environment, verify the namespace, veth,
addresses, route, and a local packet across the veth, perform explicit cleanup,
and confirm that the owned resources are absent. This test is opt-in rather
than part of default CTest and does not replace the final privileged MVP
qualification matrix. It does not require public Internet, DNS, NAT, firewall,
traffic shaping, Workload integration, or privileged fault injection.

The local packet check uses a test-only UDP echo child. The child enters the
already-owned exact namespace handle, binds `10.200.0.2`, echoes one bounded
datagram from the host endpoint, and is always reaped. A private smoke/test
accessor can borrow that handle from the opaque implementation; no descriptor
accessor is added to the module interface. This test-only process is not the
production Workload integration.

## Experimentally verified behavior

The manual experiment established that:

- veth plus addresses is sufficient for traffic between the namespace and the
  host endpoint;
- Internet access additionally requires a namespace route, host IPv4
  forwarding, NAT, DNS, and compatible firewall forwarding;
- successful public ICMP does not prove that forwarded TCP, UDP, or DNS is
  permitted;
- separate qdiscs on `nll-app` and `nll-host` can affect outbound and inbound
  traffic independently.

These findings are evidence for the target design, not implemented NetLagLab
behavior.

## DNS mechanism and open contents decision (Q64)

A network namespace changes interfaces, routes, ports, and network stack
state; it does not by itself replace filesystem paths. The accepted mechanism
is therefore a private mount namespace for the Workload with a read-only bind
mount at `/etc/resolv.conf`.

What populates that file remains open:

1. **Resolver snapshot:** build a bounded, validated, root-owned snapshot from
   host resolver files once per Session. This is smaller but may lose dynamic
   VPN split-DNS behavior.
2. **Host-side DNS proxy:** expose a resolver through the host endpoint. This
   can preserve more host/VPN behavior but adds another privileged service,
   protocol, and lifecycle.

An implementation slice that requires working DNS must obtain this decision.
It must not silently inject a public resolver such as Google or Cloudflare.

## Host route selection open decision (Q65)

Every Workload packet first uses the namespace default gateway:

```text
Workload
  -> nll-app
  -> namespace default via 10.200.0.1
  -> nll-host on the host
  -> host route-policy lookup
  -> selected Wi-Fi, Ethernet, LAN, or VPN path
```

The unresolved choice is:

1. Let current host routing choose the uplink for each forwarded packet and
   scope masquerade to source `10.200.0.2/32` without pinning an output
   interface.
2. Select and pin one host uplink at Session startup, accepting that it may
   become stale when host connectivity changes.

Forwarded packets do not necessarily inherit UID-, cgroup-, or mark-based VPN
policy used for locally originated user traffic. The product expectation for
that case must be decided before production routing, NAT, or firewall rules
are implemented.

## Cleanup

The helper removes only resources it created. Namespace-name deletion is not
proof that the underlying kernel namespace has been destroyed: an unmanaged
descendant may retain an anonymous namespace after the name and veth are gone.
The complete contract is in [Cleanup and recovery](cleanup-and-recovery.md).
