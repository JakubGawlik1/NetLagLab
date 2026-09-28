# NetLagLab agent guidance

## Project records

- Read `PROJECT.md` before non-trivial planning, implementation, or review. It
  is a concise description of the product, verified capabilities, stable
  constraints, and possible future capabilities.
- Treat code, tests, and configuration as the source of truth for implemented
  behavior. Treat future items and their order as planning prompts, not task
  boundaries or reasons to defer coherent work requested by the user.
- Define the exact scope and implementation order with the user when starting
  the work. Keep detailed technical explanations in focused documents and link
  them from the project context when useful.

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

## C++ and build conventions

- Use C++20 without compiler-specific extensions.
- Prefer the simplest design that keeps responsibilities, ownership, lifetime,
  and boundaries clear.
- A class, file, function, or abstraction is justified when it provides a
  concrete current benefit for correctness, clarity, cohesion, ownership,
  meaningful duplication, or testability. Their count is a consequence of the
  design, not a goal.
- When two designs are similarly correct and clear, follow nearby code and
  choose the one requiring fewer concepts to understand.
- Prefer explicit control flow and concrete types over clever ranges pipelines,
  metaprogramming, traits, or functional composition when both designs express
  the same responsibility equally well.
- A non-obvious standard-library type or function is allowed when it provides a
  concrete correctness, lifetime, ownership, or clarity benefit. Explain that
  benefit and compare it briefly with the simplest familiar alternative.
- Do not weaken RAII, type safety, lifetime safety, validation, or error
  handling merely to reduce the number of lines.
- Optimize performance only for an explicit requirement or measured
  bottleneck.
- Use target-based CMake. Keep `project(... VERSION ...)` as the single source
  of the application version.
- Keep `-Wall -Wextra -Wpedantic` clean. Support GCC and Clang, but validate
  with either one unless the task requires both.
- Prefer Ninja when available; otherwise use CMake's default generator.
- Prefer the standard library and ask before adding a production dependency.
- Use GoogleTest for unit tests and CTest as the test runner when tests are
  introduced.
- Comments should explain non-obvious reasons, not restate code.

## Workflow

- For a small, isolated task with explicit acceptance criteria, make routine
  implementation decisions autonomously.
- For non-trivial work, first present the proposed design, affected boundaries,
  and acceptance criteria in at most six concise bullets. Stop and wait for
  explicit approval before modifying files.
- Implement the smallest complete solution. Include all work necessary for
  correctness and integration, but avoid unrelated cleanup, speculative
  features, premature generalization, and large code dumps.
- If the expected scope must materially expand, explain why before editing
  additional components.
- Do not install packages or change persistent host networking unless the user
  explicitly requests it.
- After implementation, briefly explain only non-obvious C++ choices,
  ownership or lifetime constraints, and meaningful failure paths.
- Do not paste complete source files unless asked.

## Test scope

- Separate adding tests from running existing tests.
- Add a test only for a new or fixed observable contract not already covered.
  Usually one focused regression test is enough for one behavior.
- Do not duplicate the same contract across several test layers or generate
  option combinations that the requirements do not define.
- Do not create a new CLI test harness solely for a trivial flag. Use focused
  executable smoke checks unless an existing CLI test target naturally covers
  the behavior.
- For an isolated change in the CLI executable, build `netlaglab` and run smoke
  checks for the changed behavior. Do not run unrelated
  `netlaglab_core_tests`.
- For a change in `netlaglab_core`, build its consumers and run the smallest
  relevant CTest filter first.
- Run the full CTest suite when shared core behavior, public headers, CMake,
  compiler options, or dependencies changed; when focused validation fails or
  reveals coupling; before a release; or when explicitly requested.

## Code review rules

### Scope and complexity

- Flag a file, abstraction, configuration change, dependency, or test only when
  it lacks a concrete requirement, useful responsibility, or useful boundary;
  do not flag it merely because it increases the count.
- Distinguish correctness issues from optional simplification or style
  suggestions. Do not apply optional suggestions automatically.

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
  `nll-host` egress is application inbound traffic.
- Ping measures a round trip and cannot prove direction by itself. Test each
  direction with different settings when direction matters.

## Validation commands

Configure when the build directory is absent or CMake configuration changed:

```bash
cmake -S . -B build -G Ninja
```

Build only the affected target when practical:

```bash
cmake --build build --target netlaglab
cmake --build build --target netlaglab_core_tests
```

Run a focused test selection when applicable:

```bash
ctest --test-dir build -R '<relevant-test-regex>' --output-on-failure
```

Run the full suite only under the conditions defined above:

```bash
ctest --test-dir build --output-on-failure
```

Check the resulting diff:

```bash
git diff --check
```

If Ninja is unavailable, omit `-G Ninja`.

## Agent skills

### Issue tracker

Issues and specs are tracked in this repository's GitHub Issues. See
`docs/agents/issue-tracker.md`.

### Domain docs

This repository uses a single-context domain-doc layout. See
`docs/agents/domain.md`.
