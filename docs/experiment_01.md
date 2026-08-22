# Experiment 01: Network namespace and traffic shaping

## Goal

The goal was to verify whether it is possible to isolate an application inside
a Linux network namespace and change its network conditions with `tc/netem`.

## Topology

```text
host namespace                         nll-demo namespace

nll-host                              nll-app
10.200.0.1/30  <------ veth ------>   10.200.0.2/30
```

`nll-host` acts as the gateway for the isolated namespace.

## Traffic directions

Outbound traffic from the application follows this path:

```text
nll-app egress -> nll-host ingress
```

Inbound traffic to the application follows the opposite path:

```text
nll-host egress -> nll-app ingress
```

A root qdisc affects egress traffic. This means that separate qdiscs on
`nll-app` and `nll-host` can emulate different outbound and inbound network
conditions.

## Preparing the namespace

I created a network namespace and a pair of connected virtual Ethernet
interfaces:

```bash
sudo ip netns add nll-demo

sudo ip link add nll-host type veth peer name nll-app
sudo ip link set nll-app netns nll-demo

sudo ip -n nll-demo addr add 10.200.0.2/30 dev nll-app
sudo ip addr add 10.200.0.1/30 dev nll-host

sudo ip link set nll-host up
sudo ip -n nll-demo link set nll-app up
sudo ip -n nll-demo link set lo up
```

This is sufficient for communication between the namespace and the host-side
endpoint:

```bash
sudo ip netns exec nll-demo ping 10.200.0.1
```

The loopback interface is enabled because applications may use `localhost` for
communication between processes inside the same namespace.

## Internet access

Accessing the internet required three separate mechanisms:

1. A default route inside the namespace.
2. IPv4 forwarding on the host.
3. NAT for translating the private namespace address to the host address.

I added the default route through `nll-host`:

```bash
sudo ip -n nll-demo route add default via 10.200.0.1 dev nll-app
```

I verified that IPv4 forwarding was enabled:

```bash
sysctl net.ipv4.ip_forward
```

The following nftables table provided source NAT for the experiment:

```bash
sudo nft add table ip nll_lab
sudo nft 'add chain ip nll_lab postrouting { type nat hook postrouting priority srcnat; policy accept; }'
sudo nft 'add rule ip nll_lab postrouting ip saddr 10.200.0.0/30 masquerade'
```

The masquerade rule replaces the private source address from
`10.200.0.0/30` before packets leave the host. DNS was configured separately
for the namespace by using an IPv4 resolver in
`/etc/netns/nll-demo/resolv.conf`.

Some of the NAT and firewall commands were prepared with AI assistance. I ran
them locally and verified their behaviour through controlled tests.

## Traffic shaping results

I used `tc/netem` on both ends of the veth pair. For example, the following
command added 100 ms of outbound delay:

```bash
sudo ip netns exec nll-demo \
  tc qdisc replace dev nll-app root netem delay 100ms
```

The corresponding command on `nll-host` affected inbound traffic:

```bash
sudo tc qdisc replace dev nll-host root netem delay 100ms
```

My observations were:

- A fixed delay of 100 ms on `nll-app` increased ping RTT by approximately
  100 ms.
- Adding another 100 ms on `nll-host` increased the total RTT to approximately
  200 ms.
- `delay 100ms 30ms` caused the RTT to vary and produced an `mdev` of about
  16.5 ms.
- `loss 20%` randomly removed some packets instead of delaying them.

Ping initially made the traffic directions confusing because it reports RTT.
An ICMP Echo Request travels in one direction, while a separate Echo Reply
travels back in the other direction, so the displayed time includes both
qdiscs.

## Firewall problem

Pings to public IPv4 addresses worked, but DNS queries and TCP connections
timed out. This initially looked like a DNS configuration problem.

The actual cause was UFW. It had forwarding rules for `virbr0`, but not for
`nll-host`. Its default rules allowed diagnostic ICMP traffic while new
forwarded TCP and UDP connections were blocked.

I added a scoped route rule for traffic entering the host through `nll-host`,
leaving through the external network interface, and originating from
`10.200.0.0/30`. After adding this rule, DNS and HTTPS worked from inside the
namespace.

This demonstrated that a successful ping confirms ICMP connectivity, but does
not prove that TCP and UDP traffic are allowed.

## Cleanup

The namespace, veth pair, nftables table, DNS configuration and temporary UFW
rule are runtime resources and should be removed after the experiment. A
restart also removes the namespace, veth pair and qdiscs, but the UFW rule is
persistent and must be deleted explicitly.

## What I learned

- How to create an isolated network namespace.
- How to create a veth pair and move one endpoint into another namespace.
- How inbound, outbound, ingress and egress describe traffic direction.
- Why separate qdiscs are needed for asymmetric network conditions.
- The difference between fixed delay, jitter and packet loss.
- Why ping RTT includes the path in both directions.
- How routing, IP forwarding and NAT provide internet access to a namespace.
- Why a host firewall must be considered in network-dependent software.

The most important lesson was that NetLagLab must detect or clearly report host
firewall restrictions. Otherwise users could mistake a firewall configuration
problem for a failure in NetLagLab or in the application being tested.
