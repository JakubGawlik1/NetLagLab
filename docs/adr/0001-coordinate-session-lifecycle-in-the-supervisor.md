---
status: accepted
---

# Coordinate the Session lifecycle in one Supervisor-side module

The Supervisor exposes one deep coordination boundary for a Session and owns
only its side of that lifecycle: the `sudo` launcher, helper conversation,
Controller endpoint, local lifecycle state, and Session Outcome. The helper
remains the sole owner of the directly managed Workload, privileged resources,
reaping, and privileged cleanup. We chose this over a state machine shared
across both processes or caller-visible lifecycle phases because it hides
ordering from callers without conflating ownership across the privilege and
process boundary. Its caller uses one blocking operation that returns only after
terminal cleanup and launcher reaping; startup, activation, supervision,
shutdown, and reaping are not caller-driven phases.
