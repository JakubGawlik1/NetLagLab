---
status: accepted
---

# Use one event-action Controller control plane

The Supervisor uses one Session-scoped `ControllerControlPlane` whose interface
accepts Controller, helper-profile, stopping, and terminal events and returns
ordered typed actions. It replaces the socket-bound `ControllerConversation`
and hides command framing, operational presentation, queueing, reply
association, confirmed profile, and public state; the production lifecycle
adapter only owns descriptors, single-slot transport admission, polling,
action execution, and publication of the final typed Session Outcome after
cleanup. An extra accepted descriptor is rejected before it becomes a
control-plane event. We chose this over adding a second state class or
putting the logic directly in the production adapter because both alternatives
would spread the same ordering rules across modules and force tests to reach
through socket I/O or private state. The interface is one event-in/action-out
operation. Action batches execute synchronously in order: infallible lifecycle
requests first, Controller sends next, and at most one helper dispatch last.
Controller write failure is a distinct feedback event that discards unexecuted
actions without confusing them with an already accepted Profile Change. This
keeps partial side effects explicit without exposing internal state or adding a
success acknowledgement for every action.
