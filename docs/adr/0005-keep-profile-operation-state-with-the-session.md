---
status: accepted
---

# Keep Profile Change state with the Session

The Session owns its last helper-confirmed Network Profile and the one Profile
Change dispatched to the helper, while a Controller connection owns only its
undispatched queue and reply entitlement. Disconnect therefore discards queued
commands but does not cancel an in-flight change; its result may update the
Session without being delivered to a replacement Controller. We chose this over
connection-owned operation state because the privileged mutation and confirmed
profile outlive an optional UI connection, and over retaining the whole queue
because commands that were never dispatched have no Session-level effect or
valid recipient for their replies.
