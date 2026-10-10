---
status: accepted
---

# Use a bounded DNS snapshot for each Session

The Workload receives a root-owned, bounded snapshot of supported host IPv4
resolver configuration through read-only `/etc/resolv.conf` and
`/etc/nsswitch.conf` bind mounts in its private mount namespace. Host lookups
use `files` and `dns`; loopback resolvers, systemd-resolved routing, split-DNS,
proxy-only behavior, and unsupported resolver directives fail before
activation with a diagnostic. This avoids a host-side DNS proxy and never
substitutes an invented public resolver, at the cost of refusing configurations
whose routing semantics cannot be represented by a static snapshot.
