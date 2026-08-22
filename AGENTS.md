# NetLagLab agent guidance

## Project

- NetLagLab is a Linux application that runs a local application in an isolated
  network environment and changes its network conditions live.
- This is an educational C++ portfolio project for internship preparation.
  Prefer code the owner can understand and explain over clever abstractions.
- The MVP targets Linux and IPv4, one local process, and one active session.
  Build a CLI first; add Qt only after the same operations work without a GUI.
- Use a network namespace and veth pair as the initial isolation backend. The
  veth pair is an implementation detail, not a user-facing choice.
- The product should support separate outbound and inbound latency, jitter,
  packet loss, and bandwidth settings with live updates. Persistence should
  record the timestamped settings timeline in JSON Lines for later replay.
- The current task defines the scope. Do not prepare later features unless the
  task explicitly requires it.

## Architecture and safety

- Keep domain and session logic independent of CLI and GUI code. Both frontends
  should call the same typed operations.
- Keep privileged Linux operations behind a narrow system adapter or helper and
  apply least privilege.
- Validate user-controlled values. Never place them in a shell command string;
  invoke an executable with separate arguments.
- Make setup and cleanup idempotent. Partial failure or interruption must not
  silently leave namespaces, veth devices, qdiscs, NAT rules, DNS files, or
  temporary firewall rules behind.
- Never disable the host firewall or add broad permanent rules. Prefer
  diagnostics; any automatic rule must be scoped, explicit, temporary, and
  removable.
- Before work involving namespaces, routing, `tc`, DNS, NAT, or firewalls,
  read `docs/experiment_01.md`.

## Technical conventions

- Use C++20 without compiler-specific extensions.
- Use target-based CMake. Keep `project(... VERSION ...)` as the single source
  of the application version.
- Keep `-Wall -Wextra -Wpedantic` clean. Support GCC and Clang, but validate
  with either one unless the task requires both.
- Prefer Ninja when available; otherwise use CMake's default generator.
- Keep the repository small. Add a directory, library, abstraction, or
  dependency only when the current task needs it.
- Prefer the standard library and ask before adding a production dependency.
- Use GoogleTest for unit tests and CTest as the test runner when tests are
  introduced.
- Comments should explain non-obvious reasons, not restate code.

## Workflow

- Inspect Git status and relevant files before editing. Preserve user changes.
- For non-trivial work, first explain the intended change and relevant C++ or
  Linux concepts in at most six concise bullets.
- Make the smallest coherent change that meets the acceptance criteria. Avoid
  unrelated cleanup, premature generalization, and large code dumps.
- Do not commit, push, install packages, or change persistent host networking
  unless the user explicitly requests it.
- Build after editing and run relevant tests or smoke checks. Report commands,
  results, and anything that remains unverified.
- Explain changed files, key choices, and failure handling for a learner. Do not
  paste complete files unless asked.

## Code review rules

### Command execution

- Flag user input inside shell command strings. Safe path: validate the input
  and pass the executable and every argument separately.

### Privileged resource lifetime

- Flag privileged setup without cleanup for each partial-failure path. Safe
  path: track created resources and remove only those resources idempotently.
  Never fix forwarding by disabling the firewall or adding an unrestricted
  persistent rule.

### Traffic direction

- A root qdisc shapes egress. `nll-app` egress is application outbound traffic;
  `nll-host` egress is application inbound traffic. Ping measures a round trip
  and cannot prove direction by itself. Safe path: test each direction with
  different settings.

## Validation

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run CTest only when tests exist. If Ninja is unavailable, omit `-G Ninja`.
