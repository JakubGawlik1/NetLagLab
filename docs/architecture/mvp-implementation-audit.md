# MVP implementation audit — updated 2026-10-02

## Privileged Session qualification — 2026-10-08

The opt-in `netlaglab_session_qualification` CTest was run directly as root on
Arch Linux kernel `7.2.9-arch1-1` using
`NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1`. CTest exited successfully with three
cases passed and one skipped:

- **Controller stop:** passed. The directly managed Workload observed
  termination, the Controller received the final Session Outcome, and cleanup
  released the owned topology and host lock.
- **Supervisor loss:** passed. The helper stopped and reaped the Workload, and
  cleanup released the owned topology and host lock.
- **Failed exec:** passed. The failure result was preserved and prepared
  topology/lock cleanup completed.
- **Natural exit:** the test verified Workload context, exit code `37`, and
  topology/lock cleanup. It then skipped the UDP assertion: the host peer
  received no reply even though the packet was observed at `nll-host`. This
  host's UDP receive path therefore remains unqualified. The skip is not
  counted as a successful UDP exchange.

No firewall changes were made. This run verifies the integrated Session
namespace and lifecycle/cleanup paths on this host, subject to the UDP receive
limitation. It does not verify UDP echo, public connectivity, DNS, NAT,
firewall integration, or shaping. The reproducible test and its explicit skip
reason are in `tests/session_qualification_test.cpp`.

## Implementation update — 2026-10-02

The standalone private Network Environment library can now create and prove
the fixed namespace and veth resource roots, move `nll-app` through the exact
owned namespace handle, configure both addresses, all three required link
states, and the namespace default route, and perform proof-based cleanup.
Namespace-side mutations enter the exact retained handle in bounded child
processes. This implementation is covered by controlled unprivileged tests
only. It is not linked into the helper and has not passed the separately
authorized privileged qualification.

## Implementation update — 2026-09-29

Issue #4 is implemented in commit `808e779`. The Session-scoped
`ControllerControlPlane` replaces `ControllerConversation` and owns framing,
the 32-command queue, reply generations, the confirmed Network Profile, and
public running/stopping state through one event/action interface. Production
dispatches typed Profile Changes through the helper and explicitly completes
them as restored-after-failure until real shaping exists. Dedicated
`profile_state` lifecycle/outcome handling, the production action executor,
and an injected in-memory successful adapter path are covered by focused tests.

## Implementation update — 2026-09-28

At the 2026-09-28 checkpoint, the Controller parser from issue #1 had been
implemented. A pure `parse_controller_command()` seam recognizes all
six command families and returns typed commands, an ignored-line marker, or a
structured error. It validates the 1024-byte raw-line limit, ASCII/tab input,
directions, settings, numeric grammar, units, ranges, overflow, and loss
underflow. `ControllerConversation` then mapped those results to fixed
responses; valid `set`/`reset` requests truthfully remained unavailable until
helper dispatch existed.

At the same checkpoint, issue #2 had also been implemented. A shared bounded
terminal protocol preserves the typed Workload result and every infrastructure
failure for an attached Controller. Supervisor cleanup finishes before the
outcome is published, send failure cannot mutate the result, and the attach
client retains legacy terminal-input compatibility. These changes completed the
parser and terminal-result portions of the former Controller slice. At that
point, Controller queuing, dispatch and consumption of helper acknowledgements,
confirmed-profile state, and stopping status remained unimplemented; the
2026-09-29 update above records their later integration.

Issue #3 was implemented in commit `743b702`. `ProfileChange` and all set/reset
alternatives belong to the shared domain model, and `apply_profile_change()`
validates and applies one change without partial mutation. The typed
Supervisor/helper conversations now round-trip the fixed Profile Change
vocabulary, validate canonical values independently at the helper boundary,
enforce one operation in flight, preserve stop priority and stream framing, and
distinguish applied, restored-after-failure, and terminal unknown-state results.
This completed the former protocol slice. At that checkpoint, Controller
dispatch and queuing, confirmed public profile updates, and the privileged
shaping adapter remained unimplemented. Commit `808e779` later integrated the
Controller responsibilities; the privileged shaping adapter is still absent.

## Executive conclusion

The current working tree is **not yet the NetLagLab MVP**. It has a substantial
and tested process-control foundation: the Supervisor starts and authenticates
the privileged helper, the helper launches and reaps the Workload under the
invoking identity, the lifecycle preserves execution and infrastructure
outcomes, and the Controller handles the complete public command set and
dispatches typed Profile Changes. However, the Workload still runs in the host
namespaces, the completed standalone host-local topology is not connected to
the helper, DNS/mount and host-connectivity work remains absent, and no
production code applies a Network Profile. The active Session therefore still
has no network isolation or shaping.

Using the accepted architecture responsibilities as the unit of counting, **5
coherent implementation slices remain**. After those slices, **1 privileged
end-to-end qualification gate** still has to pass before the MVP can be called
verified. Three accepted design decisions block parts of that work: DNS
contents (Q64), host-route selection (Q65), and persistent recovery (Q47).

This count is deliberately not a count of files, classes, commands, or system
calls. It groups work by independently testable responsibility and therefore
does not pretend that all five slices have equal size.

## Audit scope and evidence policy

- Original snapshot: branch `main`, commit `633207b`, including every staged,
  unstaged, and untracked file visible on 2026-09-25.
- Update evidence: local branch `main` through the standalone fixed-local-
  topology implementation on 2026-10-02. The result remains a local assessment
  rather than a released-version assessment.
- Primary sources only: production code, tests, CMake configuration, repository
  architecture/product documents, the recorded manual experiment, and Git.
- Documentation claims were accepted only when corroborated by current code or
  explicitly labelled as target design/manual evidence. The architecture index
  itself defines these evidence categories at
  `docs/architecture/README.md:7-20`.
- The 2026-09-29 absence check searched production and test sources for `setns`,
  `unshare`, `CLONE_NEWNET`, `CLONE_NEWNS`, `netns`, `veth`, `nll-host`,
  `nll-app`, `nft`, `ufw`, `firewalld`, `qdisc`, `netem`, `resolv.conf`,
  `ip_forward`, and `masquerade`. Profile-mutation frames are now implemented as
  an unprivileged conversation contract. That result is superseded for
  standalone namespace/veth creation and fixed local configuration by the
  2026-10-02 update above; DNS/mount, host connectivity, and Session integration
  remain absent. The Workload still uses the host network and Controller status
  still reports `shaping: not applied`.

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
   flight (`docs/architecture/controller-control-plane.md:241-317`).
7. Startup, mutation, shutdown, rollback, and cleanup are transactional.
   NetLagLab removes only resources it can prove it owns, and unknown or
   unclean state is infrastructure failure 125
   (`docs/architecture/cleanup-and-recovery.md:15-53,55-75`).

`PROJECT.md` lists JSON Lines recording/replay and a GUI as candidate
capabilities rather than verified behavior or fixed milestones
(`PROJECT.md:57-69`). This audit excludes both from the smallest MVP because the
product goal and the accepted architecture contract above close without them.
If settings-history recording is promoted into the MVP, it adds one further
implementation slice. The GUI is ordered after the CLI/control path in the
current product context. Multiple Sessions, IPv6, dynamic subnet allocation,
and cross-platform support are also outside this count (`PROJECT.md:23-25`,
`docs/architecture/network-environment.md:32-33`).

The public issue tracker is not a complete MVP backlog. Issues
[#1 — Controller parser](https://github.com/JakubGawlik1/NetLagLab/issues/1),
[#2 — attached Controller Session Outcome](https://github.com/JakubGawlik1/NetLagLab/issues/2),
and [#3 — typed Profile Change protocol conversations](https://github.com/JakubGawlik1/NetLagLab/issues/3)
were all closed on 2026-09-28. [Issue #4 — event-action Controller control
plane](https://github.com/JakubGawlik1/NetLagLab/issues/4) is implemented in
local commit `808e779` but remained open in the tracker when this update was
verified on 2026-09-29. These issues cover checkpoints inside the broader
responsibilities below; the missing privileged backend responsibilities do not
yet have one issue each, so issue count cannot be used as the remaining MVP
count.

## Current-state classification

| MVP area | Classification | Current evidence |
|---|---|---|
| Build, typed profile, and scope constraints | **Implemented foundation** | CMake builds the CLI, helper, core library, and six test executables. The shared domain owns every Profile Change alternative, validates complete profiles, and applies changes atomically for both directions and all four settings (`include/netlaglab/network_profile.hpp`, `src/network_profile.cpp`, `tests/network_profile_test.cpp`). |
| Supervisor/helper/Workload lifecycle | **Implemented in code; unprivileged tests passed** | `run_session()` captures context, locks the user runtime, starts and connects to the helper, then enters the typed lifecycle (`src/session.cpp:517-627`). The lifecycle owns activation, stop escalation, result precedence, cleanup, and launcher reaping (`src/session_lifecycle.cpp`). Tests cover clean and failing outcomes, Controller loss, both Ctrl-C paths, and unknown profile state (`tests/session_lifecycle_test.cpp:100-393`). |
| Helper-owned Workload execution | **Partial** | The helper authenticates the Supervisor, acquires `/run/netlaglab/host.lock`, launches, signals, and reaps the Workload (`src/helper_main.cpp`). The child restores groups/GID/UID, standard descriptors, cwd, argv/environment, and executes without a shell (`src/workload_process.cpp:189-329`). There is no namespace or mount entry before the identity drop; the accepted target requires both (`docs/architecture/workload-execution.md:40-69`). |
| Base Controller | **Implemented control-plane checkpoint; shaping pending** | `ControllerControlPlane::handle()` owns framing, the 32-command queue, reply ownership, confirmed profile, shaping flag, and running/stopping state. Typed `set`/`reset` requests dispatch to the helper; production reports reversible failure because real shaping remains absent. Complete Session Outcome delivery and legacy attach compatibility remain implemented. |
| Supervisor-helper protocol | **Implemented and connected; shaping pending** | Existing lifecycle/start/stop/cleanup behavior is preserved. Typed runtime variants include every Profile Change, exact canonical serialization, independent helper validation, one-operation ordering, fixed success/failure responses, stop priority, terminal cancellation, and byte-stream fragmentation/coalescing coverage. Production Controller dispatch uses this contract. |
| Network and mount environment | **Standalone local topology implemented; Session integration and mount/DNS pending** | The private production adapter transactionally creates and proves the fixed namespace/veth roots, assigns both addresses, brings up both endpoints and loopback, adds the namespace default route, and performs proof-based cleanup. Controlled tests cover exact commands and every new failure point. The library is not linked into the helper, the Workload remains in the host namespaces, and private mount/DNS setup is absent. |
| Host routing/NAT/firewall | **Manual experiment only; missing from production** | Experiment 01 demonstrated scoped NAT and a UFW forwarding problem/fix (`docs/experiment_01.md:66-100,133-148`). Production currently performs no routing, forwarding, NAT, or firewall mutation (`docs/architecture/host-networking-and-firewall.md:3-10`). |
| Traffic shaping | **Implemented domain/protocol/control-plane model plus manual experiment; runtime missing** | Typed validation, atomic Profile Change application, and live dispatch/result integration exist, and delay/jitter/loss were manually exercised on both veth endpoints (`docs/experiment_01.md:102-131`). Runtime qdisc application, bandwidth composition, and privileged apply/rollback are absent (`docs/architecture/traffic-shaping.md`). The manual experiment did not establish bandwidth limiting. |
| Cleanup and recovery | **Partial** | Current process, descriptor, socket, and lock cleanup exists. The standalone Network Environment owner now performs proof-based namespace/veth cleanup and configuration failures roll back those roots. Active helper integration, DNS/NAT/firewall/qdisc cleanup, persistent firewall recovery, and interrupted-state reconciliation remain absent. |

The code-level classification now matches the repository feature map for the
Controller, protocol, and standalone fixed-local-topology checkpoints. Active
Session networking, firewall, DNS/mount, and runtime shaping remain incomplete.

## The 5 remaining implementation slices

These are responsibility slices, not a mandatory commit sequence.

1. **Workload namespace and mount entry.** Extend the helper-owned launch path
   so the child enters the prepared network namespace and private DNS mount view
   before restoring user credentials and calling `execve()`. Preserve the
   already implemented stdio, argv/environment/cwd, signal, and result
   contracts. Acceptance: the Workload demonstrably has the Session interfaces,
   routes, loopback, and resolver view while retaining the invoking identity.

2. **Transactional network/mount environment.** The standalone network portion
   now implements preflight, collision refusal, ownership, namespace/veth,
   addresses, loopback, default route, and proof-based rollback. Complete the
   private mount namespace and DNS file/mount ownership within the same
   transactional contract. Acceptance: local isolation works without requiring
   public Internet, and injected failure after each remaining setup step leaves
   no owned residue.

3. **Scoped host connectivity and firewall integration.** Implement forwarding
   preflight, the Session-owned nftables NAT table, host-route policy, supported
   UFW/firewalld detection/adapters, `/dev/tty` consent, exact cleanup, and safe
   refusal for ambiguous/unsupported policy. Acceptance: supported cases provide
   DNS/TCP/UDP connectivity without disabling or broadly weakening the host
   firewall; unsupported cases fail before Workload activation.

4. **Runtime shaping backend.** Apply the unrestricted initial profile, then
   implement transactional outbound (`nll-app`) and inbound (`nll-host`)
   delay/jitter/loss/bandwidth changes with restoration of the last confirmed
   state. Acceptance: asymmetric tests prove direction; each setting, rollback,
   unknown-state failure, and bandwidth-plus-netem composition are covered.

5. **Network cleanup and interrupted recovery.** Integrate all namespace,
   mount, veth, route, NAT, firewall, and qdisc resources into normal cleanup,
   partial-start rollback, Supervisor-loss handling, idempotent retry, and the
   durable journal/reconciliation path required by persistent firewall changes.
   Acceptance: only proven-owned resources are removed, cleanup failure
   overrides a known Workload outcome with 125, and interrupted persistent state
   is reconciled under the host lock before a new Session starts.

Slice 1 extends a currently partial module. Slices 2–5 contain the remaining
privileged backend and dominate the remaining engineering risk. The standalone
host-local network portion of slice 2 is implemented; production DNS,
Internet/firewall, and recovery cannot be completed without Q64/Q65/Q47
(`docs/architecture/session-lifecycle-implementation-status.md:49-59`).

## Decisions and design work still open

The following are not counted as implementation slices, but they block parts of
the five slices:

- **Q64 — DNS contents:** bounded resolver snapshot or host-side DNS proxy
  (`docs/architecture/network-environment.md:95-112`).
- **Q65 — host route policy:** dynamic host routing or one startup-pinned uplink
  (`docs/architecture/network-environment.md:114-138`).
- **Q47 — persistent recovery journal:** path, schema, durability, transitions,
  reconciliation, and diagnostics
  (`docs/architecture/cleanup-and-recovery.md:106-132`).

The profile wire names, normalized encodings, result vocabulary, Controller
queue ownership, stop precedence, action-failure feedback, and
unknown-profile-state cleanup are implemented. Lower-level design is still
open for qdisc realization plus bandwidth composition
(`docs/architecture/traffic-shaping.md`). These are contained inside slice 4
rather than counted again.

## Verification performed

Commands run from the repository root:

```text
git status --short --branch
git log --oneline --decorate -n 20
git diff --cached --stat
git diff --stat
rg --files src tests include
rg -n '<network/runtime terms>' src tests CMakeLists.txt
cmake --build build -j2
ctest --test-dir build -R \
  'ControllerControlPlaneTest|ControlPlaneActionExecutorTest|HelperProtocolTest' \
  --output-on-failure
ctest --test-dir build --output-on-failure
git diff --check
```

Results:

- all targets built successfully with the configured warning options;
- all 33 focused Controller control-plane, action-executor, and helper-protocol
  tests passed;
- all 111 discovered tests passed outside the filesystem/network sandbox;
- `git diff --check` passed;
- the earlier sandboxed Controller socket failures remained attributable to the
  environment: the complete suite passed outside that restriction.

## Verification not performed and release caveats

No privileged `sudo` end-to-end Session was run. Therefore this audit does not
claim current verification of the real root helper path, `/run/netlaglab`
permissions, sudo/PTTY stdio behavior, terminal signal escalation, or any future
namespace/NAT/firewall/DNS/qdisc behavior. The repository records the same
boundary at `docs/architecture/session-lifecycle-implementation-status.md:61-72`.

This is the **one qualification gate not included in the five implementation
slices**: after the code slices and their focused tests exist, an explicitly
authorized privileged matrix must verify normal startup, each traffic
direction/setting, pipes and terminals, partial failures, cleanup, collisions,
Supervisor/helper loss, and interrupted recovery.

One current user-facing inconsistency should remain visible during development:
the CLI help already says that NetLagLab runs an application in an isolated
network environment (`src/main.cpp:9-16`), while the implemented program still
uses the host network (`docs/runtime_call_map.md:39-41`). Until host connectivity
and Workload namespace entry are integrated, that sentence describes the
product goal rather than current behavior.

## Counting limitations

- The five-slice number measures remaining responsibilities, not effort. Host
  firewall safety and durable recovery are materially larger and riskier than
  Workload namespace entry.
- Manual commands prove mechanism feasibility on one host snapshot, not
  production ownership, rollback, portability across supported firewall states,
  or current host compatibility.
- Passing unprivileged tests supports the process/protocol foundation but cannot
  establish privileged integration correctness.
- A narrower demo that only creates a namespace and applies one fixed delay
  could be produced with fewer slices, but it would not satisfy the accepted
  repository MVP contract: it would omit live controls, both directions and
  settings, safe host integration, and cleanup/recovery.
