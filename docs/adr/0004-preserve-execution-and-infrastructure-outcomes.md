---
status: accepted
---

# Preserve execution and infrastructure outcomes separately

A Session Outcome has an execution axis (`not started`, pre-activation start
failure, or Workload exit/signal) and an independent infrastructure axis
(success or one or more failures identified by lifecycle stage). Frontends map
that structure to their own exit status only at the presentation boundary; an
infrastructure failure may therefore override the returned Workload code with
`125` without discarding either fact. We chose this over a single integer or a
set of success booleans so cleanup, protocol, and launcher-reaping failures
remain diagnosable without expanding the public lifecycle into caller-managed
phases.
