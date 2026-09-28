---
status: accepted
---

# Keep wire protocols outside the Session lifecycle

The Session lifecycle exchanges typed commands and events with separate
Supervisor-helper conversation and Controller control-plane modules. The
conversation module owns stream framing, partial input, and legal wire ordering;
the control-plane module owns Controller grammar, response framing, and
presentation. The lifecycle owns endpoint lifetime, operation availability and
cancellation, and the resulting Session transitions. We chose these boundaries
so transport and presentation rules cannot become a second source of lifecycle
semantics.
