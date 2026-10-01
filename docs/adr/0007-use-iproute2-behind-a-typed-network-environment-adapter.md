---
status: accepted
---

# Use iproute2 behind a typed Network Environment adapter

The privileged helper creates and configures the MVP Network Environment
through a narrow adapter whose operations represent only the fixed namespace,
link, address, and route actions accepted by the domain. Its production
implementation invokes trusted iproute2 tools with separate arguments and no
shell, while direct system calls remain available where descriptor identity or
namespace entry requires them. We chose this over implementing raw rtnetlink
message construction and parsing because that would add substantial Linux
protocol machinery to the educational MVP, and over adding libnl or libmnl
because the project does not otherwise need a production networking library.
The helper resolves `ip` only from a closed list of system paths, accepts a
root-owned regular executable that is not writable by group or others, and
executes that exact path with a minimal fixed environment; it never uses an
inherited `PATH`. Every child operation is bounded, captures only bounded
diagnostic output, and is always reaped; setup and rollback have separate
budgets so a failed or stuck setup cannot consume the opportunity to clean
already-owned resources.

Semantic preflight does not parse iproute2 presentation output or infer
ownership from a failed mutation. It uses narrow read interfaces: pathname
metadata for the named namespace, `if_nametoindex()` for host link names,
`getifaddrs()` for address prefixes, and a validated read-only rtnetlink route
dump for all host routing tables. Raw rtnetlink is therefore confined to
inventory and ownership proof; all creation, configuration, and removal remain
fixed iproute2 operations behind the typed adapter. Read-only preflight does not
claim that namespace, veth, or `setns()` mutations will succeed. Those mechanisms
report a typed system failure at their first real operation.

The helper retains an open descriptor for the exact named network namespace.
Operations inside it execute in short-lived children that call `setns()` before
`execve()`; moving a link targets the namespace file represented by that
descriptor. The long-lived helper never leaves the host network namespace.

A resource-creating or peer-placement operation reports success only after
capturing its new ownership identity. If post-mutation identity capture fails,
the adapter records provisional ownership and immediately reconciles or rolls
the operation back. Failed or unverifiable rollback is retained as
identity-unconfirmed residual state rather than converted into name-based
cleanup authority. Address, link-state, loopback, and route operations create
only configuration milestones and do not require redundant full-state readback.

Exit, signal, and timeout are transport facts rather than proof of a mutation's
effect. For create, peer placement, and removal, the adapter resolves them to a
proven old state, proven new state, proven absence, or identity-unconfirmed
residual before returning to the coordinator. It never adopts a resource found
only by its expected name.

Veth proof uses `RTM_GETLINK` in the host and exact Session namespace to verify
interface indices, link kind, and reciprocal peer information before and after
placement. This avoids parsing `ip` output or adding `ethtool`, but the exact
cross-namespace attributes must be confirmed by a focused privileged experiment
before the production decoder is qualified.

The adapter satisfies an internal port of fixed, named semantic operations
rather than a generic command variant or configurable network builder. This
makes the unavoidable transaction steps explicit and gives namespace creation,
veth creation, peer placement, configuration, and cleanup their correct result
types. The production adapter and a scripted test adapter are the two concrete
adapters at this seam. Ownership proofs and the exact namespace handle are
move-only and opaque; identity-unconfirmed residuals have no cleanup operation.
The module's fixed MVP names, addresses, and route do not become caller inputs.

The coordinator owns semantic stage and ordering, while the adapter owns the
operation cause and bounded local diagnostics. In addition to tool exit,
signal, timeout, and identity failures, `system failure` represents direct
mechanism failures such as process creation, pipes, namespace handles, and
inventory reads. Absolute per-operation deadlines prevent an adapter from
renewing the separate setup and rollback budgets. Expiry prevents new work, but
a child already started is still terminated and reaped. This local transaction
does not require IPv4 forwarding; that prerequisite belongs to later host
connectivity integration.
