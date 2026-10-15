# CLI demonstration

Run these commands from the repository root. Build the two application targets:

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=OFF
cmake --build build --target netlaglab netlaglab-helper
```

The demonstration uses Python 3 from the host and makes HTTPS requests to
`https://example.com/`. Python is only a demo dependency. The Workload uses one
process, opens a new connection for each request, prints its duration or error,
pauses two seconds after each attempt, and bypasses host proxy settings so requests use the Session network path.

## Start the application

In the first terminal, as your ordinary user:

```sh
./build/netlaglab run -- python3 examples/http_probe.py
```

The program invokes `sudo` for its helper. If UFW or firewalld requires an
exception, review the displayed rule and answer `y` within 30 seconds. The helper
owns that rule and removes it when the Session ends. IPv4 forwarding must
already be enabled. A host resolver configuration containing both IPv4 and IPv6
servers is supported when it includes a usable IPv4 server; only the IPv4
servers are exposed to the Workload.

Wait for several successful HTTP responses to establish the baseline. A failure
here means application connectivity needs investigation before demonstrating
shaping. Access to a service running on the host itself is separately subject
to the host's INPUT firewall policy; the forwarding exception does not open
host services.

## Change conditions live

In a second terminal, as the same user:

```sh
./build/netlaglab attach
```

Enter one command at a time, waiting for its confirmation. Leave several HTTP
requests between changes to observe the effect:

```text
status
set outbound delay 150ms
set inbound delay 250ms
set outbound jitter 60ms
set outbound loss 10%
status
```

Delays should increase request durations. Jitter varies them. TCP retransmits
lost packets, so loss may increase durations or cause timeouts rather than
produce the same percentage of failed HTTP requests. Internet timing also
varies naturally. These request durations illustrate the effect; they do not
measure the two one-way directions independently.

Restore the connection and observe successful requests again:

```text
reset outbound loss
reset outbound jitter
reset outbound delay
reset inbound delay
status
```

End the application through the Controller:

```text
stop
```

Both terminals report the Session outcome after cleanup. A signal termination
is expected after `stop`. An explicit infrastructure-failure diagnostic means
the Session did not finish cleanly. Exit code 125 alone is ambiguous because a
Workload can return that code itself after successful cleanup. `detach` instead
leaves the Workload running.
Bandwidth limiting is not implemented; this demonstration covers delay, jitter,
and packet loss.

## Recorded live run — 2026-10-15

The production CLI was exercised on Arch Linux, kernel `7.2.9-arch1-1`, with
active UFW and the host's mixed IPv4/IPv6 resolver configuration. The user
started `run` and approved the exact forwarding exception. A Controller then
changed the running Python HTTPS Workload through ordinary `attach` commands.

The observations below use completed requests inside each phase, excluding
requests crossing a profile change:

| Profile | Request numbers | HTTP 200 | Median request time |
| --- | --- | --- | --- |
| Unrestricted | 1–12 | 12/12 | 151.6 ms |
| Outbound delay 150 ms | 20–34 | 15/15 | 755.6 ms |
| Also inbound delay 250 ms | 37–47 | 11/11 | 1757.9 ms |
| Also outbound jitter 60 ms | 50–60 | 11/11 | 1795.2 ms |
| Also inbound jitter 90 ms and loss 10% | 62–69 | 7/8 | 6770.4 ms |
| All settings reset | 78–95 | 18/18 | 160.9 ms |

With outbound jitter, observed request durations ranged from 1623.3 to
1901.2 ms. With 10% inbound loss, the host qdisc drop counter increased from
1 to 11; requests slowed down and one TLS handshake timed out. Separate short
100% outbound-loss and 100% inbound-loss phases prevented name resolution.
The settings were then reset, and successful requests resumed without
restarting the Workload. Detaching and reconnecting the Controller preserved
the same running Workload and its reset profile.

After `stop`, the Controller reported signal 15 with no infrastructure failure.
The Workload process, `netlaglab` namespace, `nll-host`, and both Session sockets
were independently checked absent. Before stopping, the host qdisc had returned
to `noqueue`. The helper reported successful privileged cleanup; a separate
root inventory of firewall/NAT state was not collected.

These results demonstrate application traffic effects and recovery on this
host. HTTPS includes multiple packet exchanges and Internet variability, so
these timings do not estimate exact one-way delay, jitter, or loss probability.
The separate controlled local UDP qualification remains incomplete.
