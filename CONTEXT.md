# NetLagLab

NetLagLab models a local workload whose network environment and conditions are
controlled for the lifetime of one session.

## Language

**Workload**:
The process started by NetLagLab from the program selected by the user.
Descendants may inherit its network environment, but NetLagLab does not own
their lifecycle and they may outlive the Session.
_Avoid_: Process tree, descendant processes when referring to the owned unit

**Session**:
One controlled execution of a Workload together with its network environment,
Network Profile, and owned resources from setup through cleanup. It completes
successfully only after those resources have been cleaned up.
_Avoid_: Run, namespace when referring to the whole lifecycle

**Session Outcome**:
The terminal outcome of a Session, preserving both the Workload outcome, when
one exists, and any infrastructure failure. It is not reduced to a single
process exit code.
_Avoid_: Session exit code, Workload result when referring to the whole Session

**Network Profile**:
The outbound and inbound network conditions most recently confirmed by the
privileged helper as current for a Session. A pending or failed change is not a
separate requested profile.
_Avoid_: Requested Network Profile, Applied Network Profile

**Profile Change**:
A request to set or reset exactly one network setting in one traffic direction.
It does not become part of the Network Profile until the helper confirms it.
_Avoid_: Profile Delta, Profile Mutation, Requested Network Profile

**Controller**:
An optional user interface for observing and changing an active Session. It
does not own Session resources, and disconnecting it does not end the Session.
_Avoid_: Session owner, Session when referring only to the control interface
