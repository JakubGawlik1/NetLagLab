# MVP implementation audit — 2026-09-25

## Implementation update — 2026-09-28

The Controller parser checkpoint from issue #1 is now implemented in the
current working tree. A pure `parse_controller_command()` seam recognizes all
six command families and returns typed commands, an ignored-line marker, or a
structured error. It validates the 1024-byte raw-line limit, ASCII/tab input,
directions, settings, numeric grammar, units, ranges, overflow, and loss
underflow. `ControllerConversation` now maps those results to fixed responses;
valid `set`/`reset` requests truthfully remain unavailable until helper
dispatch exists.

Issue #2 is also implemented in the current working tree. A shared bounded
terminal protocol preserves the typed Workload result and every infrastructure
failure for an attached Controller. Supervisor cleanup finishes before the
outcome is published, send failure cannot mutate the result, and the attach
client retains legacy terminal-input compatibility. These changes complete the
parser and terminal-result portions of slice 3. Queuing, helper acknowledgement,
confirmed-profile state, and stopping status remain unimplemented.

## Executive conclusion

The current working tree is **not yet the NetLagLab MVP**. It has a substantial
and tested process-control foundation: the Supervisor starts and authenticates
the privileged helper, the helper launches and reaps the Workload under the
invoking identity, the lifecycle preserves execution and infrastructure
outcomes, and the Controller supports the non-profile commands. However, the
Workload still runs in the host namespaces and no production code creates the
network environment or applies a Network Profile.

Using the accepted architecture responsibilities as the unit of counting, **7
coherent implementation slices remain**. After those slices, **1 privileged
end-to-end qualification gate** still has to pass before the MVP can be called
verified. Three accepted design decisions block parts of that work: DNS
contents (Q64), host-route selection (Q65), and persistent recovery (Q47).

This count is deliberately not a count of files, classes, commands, or system
calls. It groups work by independently testable responsibility and therefore
does not pretend that all seven slices have equal size.

## Audit scope and evidence policy

- Snapshot: branch `main`, commit `633207b`, including every staged, unstaged,
  and untracked file visible on 2026-09-25. The branch was 10 commits ahead of
  `origin/main` and the working tree was dirty, so this is a working-tree audit,
  not a released-version assessment.
- Primary sources only: production code, tests, CMake configuration, repository
  architecture/product documents, the recorded manual experiment, and Git.
- Documentation claims were accepted only when corroborated by current code or
  explicitly labelled as target design/manual evidence. The architecture index
  itself defines these evidence categories at
  `docs/architecture/README.md:7-20`.
- The absence check searched production and test sources for `setns`,
  `unshare`, `CLONE_NEWNET`, `CLONE_NEWNS`, `netns`, `veth`, `nll-host`,
  `nll-app`, `nft`, `ufw`, `firewalld`, `qdisc`, `netem`, `resolv.conf`,
  `ip_forward`, `masquerade`, and profile-mutation frames. In production code,
  the only related hits were the future-firewall notice in
  `src/session.cpp:554-556` and `shaping: not applied` in
  `src/controller_control_plane.cpp:91-100`.

## MVP criteria recovered from the repository

The smallest repository-supported definition of MVP is:

1. Linux/IPv4 only, one local Workload, one host-wide active Session, and at
   most one attached Controller (`PROJECT.md:16-25`,
   `docs/architecture/README.md:73-76`).
2. `netlaglab run -- <program> ...` launches the Workload through the mandatory
   authenticated root helper, but the Workload itself runs with the invoking
   user's identity and execution context (`PROJECT.md:31-46`,
   `docs/architecture/workload-execution.md:40-64`).
3. The Workload runs inside the fixed IPv4 network environment: namespace
   `netlaglab`, veth endpoints `nll-host`/`nll-app`, subnet `10.200.0.0/30`,
   routes, loopback, and a private mount view for Session DNS
   (`docs/architecture/network-environment.md:11-33,51-65`).
4. Host access is safe and scoped: preflight validates forwarding, NetLagLab
   owns its NAT resources, firewall changes use a supported adapter and explicit
   consent, and unrelated host policy is not weakened
   (`docs/architecture/host-networking-and-firewall.md:12-34,36-61,90-118`).
5. A Network Profile has independent outbound/inbound delay, jitter, loss, and
   optional bandwidth, and starts unrestricted
   (`docs/architecture/traffic-shaping.md:14-31,33-59`).
6. An attached Controller can apply live `set`/`reset` deltas; the visible
   profile changes only after helper acknowledgement, with one operation in
   flight (`docs/architecture/controller-control-plane.md:21-47,73-98`).
7. Startup, mutation, shutdown, rollback, and cleanup are transactional.
   NetLagLab removes only resources it can prove it owns, and unknown or
   unclean state is infrastructure failure 125
   (`docs/architecture/cleanup-and-recovery.md:15-53,55-75`).

`PROJECT.md` lists JSON Lines recording/replay and a GUI as candidate
capabilities rather than verified behavior or fixed milestones
(`PROJECT.md:49-61`). This audit excludes both from the smallest MVP because the
product goal and the accepted architecture contract above close without them.
If settings-history recording is promoted into the MVP, it adds one further
implementation slice. The GUI is ordered after the CLI/control path in the
current product context. Multiple Sessions, IPv6, dynamic subnet allocation,
and cross-platform support are also outside this count (`PROJECT.md:23-25`,
`docs/architecture/network-environment.md:32-33`).

The public issue tracker is not a complete MVP backlog. On 2026-09-25 it had
one open issue, [#1 — Add a typed parser for Controller
commands](https://github.com/JakubGawlik1/NetLagLab/issues/1), covering only the
parser part of slice 3. That checkpoint is implemented by the 2026-09-28 update
above, but the issue remains open until repository workflow closes it. The
missing privileged backend responsibilities below do not yet have one issue
each, so issue count cannot be used as the remaining MVP count.

## Current-state classification

| MVP area | Classification | Current evidence |
|---|---|---|
| Build, typed profile, and scope constraints | **Implemented foundation** | CMake builds the CLI, helper, core library, and six test executables. The profile types and validation cover both directions and all four settings (`include/netlaglab/network_profile.hpp:11-41`, `src/network_profile.cpp:8-43`, `tests/network_profile_test.cpp:26-135`). |
| Supervisor/helper/Workload lifecycle | **Implemented in code; unprivileged tests passed** | `run_session()` captures context, locks the user runtime, starts and connects to the helper, then enters the typed lifecycle (`src/session.cpp:477-587`). The lifecycle owns activation, stop escalation, result precedence, cleanup, and launcher reaping (`src/session_lifecycle.cpp:10-167`). Tests cover clean and failing outcomes, Controller loss, and both Ctrl-C paths (`tests/session_lifecycle_test.cpp:73-297`). |
| Helper-owned Workload execution | **Partial** | The helper authenticates the Supervisor, acquires `/run/netlaglab/host.lock`, launches, signals, and reaps the Workload (`src/helper_main.cpp:525-692`). The child restores groups/GID/UID, standard descriptors, cwd, argv/environment, and executes without a shell (`src/workload_process.cpp:189-329`). There is no namespace or mount entry before the identity drop; the accepted target requires both (`docs/architecture/workload-execution.md:40-69`). |
| Base Controller | **Partial** | `help`, `status`, `stop`, and `detach` retain their behavior. The pure parser recognizes typed `set`/`reset` Profile Changes, normalizes their values, and produces structured errors; the conversation reports valid changes as unavailable. The attached Controller now receives the complete typed Session Outcome after cleanup, while retaining legacy input compatibility. Status still constructs a fresh unrestricted profile and says shaping is not applied. There is no Profile Change queue, helper dispatch, or helper-confirmed profile state. |
| Supervisor-helper protocol | **Partial** | Byte-stream framing, start block, activation, stop, Workload result, and cleanup frames exist (`src/helper_protocol.cpp:46-184,195-290`). Runtime commands are restricted to `STOP TERM`, `STOP KILL`, and compatibility `SHUTDOWN` (`src/helper_protocol.cpp:102-159`). Profile delta and acknowledgement vocabulary is absent (`docs/architecture/supervisor-helper-protocol.md:3-9,178-185`). |
| Network and mount environment | **Manual experiment only; missing from production** | The manual commands created a namespace, veth, addresses, loopback, route, NAT, and DNS file (`docs/experiment_01.md:37-61,66-100`). The current target document states that production creates none of these (`docs/architecture/network-environment.md:3-9`). Source search found no production implementation. |
| Host routing/NAT/firewall | **Manual experiment only; missing from production** | Experiment 01 demonstrated scoped NAT and a UFW forwarding problem/fix (`docs/experiment_01.md:66-100,133-148`). Production currently performs no routing, forwarding, NAT, or firewall mutation (`docs/architecture/host-networking-and-firewall.md:3-10`). |
| Traffic shaping | **Partial model plus manual experiment; runtime missing** | Typed validation exists, and delay/jitter/loss were manually exercised on both veth endpoints (`docs/experiment_01.md:102-131`). Runtime qdisc application, bandwidth composition, live mutation, acknowledgement, and rollback are absent (`docs/architecture/traffic-shaping.md:3-12,85-129`). The manual experiment did not establish bandwidth limiting (`docs/architecture/traffic-shaping.md:5-9`). |
| Cleanup and recovery | **Partial** | Current process, descriptor, socket, and lock cleanup exists: the helper releases the lock and reports cleanup after reaping (`src/helper_main.cpp:442-474`), while the Supervisor reaps the launcher and removes `control.sock` (`src/session.cpp:373-397`). Network-resource rollback, persistent firewall recovery, and interrupted-state reconciliation cannot exist yet because those resources are not created (`docs/architecture/cleanup-and-recovery.md:3-13,39-53,106-132`). |

The code-level classification agrees with the repository's feature map:
`docs/architecture/README.md:36-47` marks the lifecycle checkpoint as
implemented, Workload/protocol/Controller/shaping/cleanup as partial, and the
network/firewall backend as manually verified only.

## The 7 remaining implementation slices

These are responsibility slices, not a mandatory commit sequence.

1. **Workload namespace and mount entry.** Extend the helper-owned launch path
   so the child enters the prepared network namespace and private DNS mount view
   before restoring user credentials and calling `execve()`. Preserve the
   already implemented stdio, argv/environment/cwd, signal, and result
   contracts. Acceptance: the Workload demonstrably has the Session interfaces,
   routes, loopback, and resolver view while retaining the invoking identity.

2. **Supervisor-helper profile operation protocol.** Add a fixed typed delta
   vocabulary, acknowledgements/failures, legal ordering, one-operation-in-flight
   state, terminal-event cancellation, and safe error exposure. Acceptance:
   fragmented/coalesced stream tests prove that status changes only after a
   successful helper acknowledgement and impossible ordering fails safely.

3. **Controller profile control plane.** The `set`/`reset` parser, units, exact
   normalization/overflow checks, fixed error presentation, and command-size
   policy are implemented. Add the serialized queue, helper dispatch,
   stopping state, and confirmed-profile status. The typed terminal result is
   implemented.
   Acceptance for the remainder: conversation tests cover queue ordering,
   acknowledgements, disconnects, and Workload-exit races.

4. **Transactional network/mount environment.** Implement semantic preflight,
   collision refusal, an ownership ledger, namespace/veth creation, addresses,
   loopback, default route, private mount namespace, DNS file/mount, and reverse
   rollback of every successfully created prefix. Acceptance: local isolation
   works without requiring public Internet, and injected failure after each
   setup step leaves no owned residue.

5. **Scoped host connectivity and firewall integration.** Implement forwarding
   preflight, the Session-owned nftables NAT table, host-route policy, supported
   UFW/firewalld detection/adapters, `/dev/tty` consent, exact cleanup, and safe
   refusal for ambiguous/unsupported policy. Acceptance: supported cases provide
   DNS/TCP/UDP connectivity without disabling or broadly weakening the host
   firewall; unsupported cases fail before Workload activation.

6. **Runtime shaping backend.** Apply the unrestricted initial profile, then
   implement transactional outbound (`nll-app`) and inbound (`nll-host`)
   delay/jitter/loss/bandwidth changes with restoration of the last confirmed
   state. Acceptance: asymmetric tests prove direction; each setting, rollback,
   unknown-state failure, and bandwidth-plus-netem composition are covered.

7. **Network cleanup and interrupted recovery.** Integrate all namespace,
   mount, veth, route, NAT, firewall, and qdisc resources into normal cleanup,
   partial-start rollback, Supervisor-loss handling, idempotent retry, and the
   durable journal/reconciliation path required by persistent firewall changes.
   Acceptance: only proven-owned resources are removed, cleanup failure
   overrides a known Workload outcome with 125, and interrupted persistent state
   is reconciled under the host lock before a new Session starts.

Slices 1–3 extend currently partial modules. Slices 4–7 are the missing
privileged backend and dominate the remaining engineering risk. Slice 4 can be
implemented and tested for host-local isolation before Q64/Q65/Q47 are closed;
production DNS/Internet/firewall/recovery cannot be completed without those
decisions (`docs/architecture/session-lifecycle-implementation-status.md:49-59`).

## Decisions and design work still open

The following are not counted as implementation slices, but they block parts of
the seven slices:

- **Q64 — DNS contents:** bounded resolver snapshot or host-side DNS proxy
  (`docs/architecture/network-environment.md:95-112`).
- **Q65 — host route policy:** dynamic host routing or one startup-pinned uplink
  (`docs/architecture/network-environment.md:114-138`).
- **Q47 — persistent recovery journal:** path, schema, durability, transitions,
  reconciliation, and diagnostics
  (`docs/architecture/cleanup-and-recovery.md:106-132`).

Lower-level design is also still open for profile wire names/error vocabulary
(`docs/architecture/supervisor-helper-protocol.md:178-185`), Controller error
frames/queue representation (`docs/architecture/controller-control-plane.md:124-130`),
and qdisc realization plus bandwidth composition
(`docs/architecture/traffic-shaping.md:123-130`). These are contained inside
slices 2, 3, and 6 rather than counted again.

## Verification performed

Commands run from the repository root:

```text
git status --short --branch
git log --oneline --decorate -n 20
git diff --cached --stat
git diff --stat
rg --files src tests include
rg -n '<network/runtime terms>' src tests CMakeLists.txt
cmake --build build --target netlaglab netlaglab-helper netlaglab_core_tests \
  netlaglab_lifecycle_tests netlaglab_protocol_tests \
  netlaglab_controller_protocol_tests netlaglab_process_tests
ctest --test-dir build --output-on-failure
./build/netlaglab --version
./build/netlaglab --help
```

Results:

- all requested targets were current and built successfully;
- all 46 discovered tests passed outside the filesystem/network sandbox;
- the first sandboxed CTest run passed 44/46; its two Controller socketpair
  tests failed at `send()` with `EPERM`, then both passed outside the sandbox,
  so this was an execution-environment restriction rather than a reproduced
  project defect;
- CLI smoke checks returned `NetLagLab 0.1.0` and the expected help text.

## Verification not performed and release caveats

No privileged `sudo` end-to-end Session was run. Therefore this audit does not
claim current verification of the real root helper path, `/run/netlaglab`
permissions, sudo/PTTY stdio behavior, terminal signal escalation, or any future
namespace/NAT/firewall/DNS/qdisc behavior. The repository records the same
boundary at `docs/architecture/session-lifecycle-implementation-status.md:61-72`.

This is the **one qualification gate not included in the seven implementation
slices**: after the code slices and their focused tests exist, an explicitly
authorized privileged matrix must verify normal startup, each traffic
direction/setting, pipes and terminals, partial failures, cleanup, collisions,
Supervisor/helper loss, and interrupted recovery.

One current user-facing inconsistency should remain visible during development:
the CLI help already says that NetLagLab runs an application in an isolated
network environment (`src/main.cpp:9-16`), while the implemented program still
uses the host network (`docs/runtime_call_map.md:38-40`). Until slice 4 and
Workload namespace entry are integrated, that sentence describes the product
goal rather than current behavior.

## Counting limitations

- The seven-slice number measures remaining responsibilities, not effort. Host
  firewall safety and durable recovery are materially larger and riskier than
  extending the Controller parser.
- Manual commands prove mechanism feasibility on one host snapshot, not
  production ownership, rollback, portability across supported firewall states,
  or current host compatibility.
- Passing unprivileged tests supports the process/protocol foundation but cannot
  establish privileged integration correctness.
- A narrower demo that only creates a namespace and applies one fixed delay
  could be produced with fewer slices, but it would not satisfy the accepted
  repository MVP contract: it would omit live controls, both directions and
  settings, safe host integration, and cleanup/recovery.
