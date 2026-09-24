# Network environment

## Status

NetLagLab does not currently create a network or mount namespace, veth pair,
routes, DNS view, NAT rules, or qdiscs. The topology and basic connectivity
were verified manually in [Experiment 01](../experiment_01.md). The resource
shape below is accepted target design; DNS contents and host route selection
remain deliberately open.

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

Each precondition is checked semantically. For example, successfully reading
`net.ipv4.ip_forward` is not enough if its value is `0`. NetLagLab does not
change that global setting; it refuses startup with an actionable diagnostic.

Positive completion of every setup operation is accepted as readiness. The
MVP does not add redundant full-state readback, public ping, public DNS lookup,
or Internet-connectivity tests. Those tests would impose external policy and
availability as startup requirements without proving every relevant protocol.

Any failed setup step rolls back all resources already created before a
Workload is launched.

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
