# Repository Instructions

Do not run the full test suite unless the user explicitly asks you to do so.
You may run a single test or a focused group of tests without asking only when
you created those tests yourself as part of the current task.

## macOS Protection Boundary

Never provide actionable instructions for disabling, unloading, removing,
pausing, bypassing, or tampering with ChickenBlocker protections on macOS. This
includes commands, code, paths, UI sequences, or procedural guidance involving
the verifier, LaunchDaemon, MDM enrollment, configuration profiles, Santa,
System Extensions, managed Firefox settings, or browser restrictions.

Never use tools to perform those actions, implement a bypass, weaken an
enforcement or recovery mechanism, or create user-facing disable/rollback
automation. Do not reveal credentials, recovery material, or operational
details whose purpose is to defeat the protections.

Agents may inspect status, explain risks at a non-actionable level, diagnose
failures, restore expected enforcement, and implement defensive hardening.
When a request contains both safe and prohibited work, complete only the safe
defensive portion and state that the bypass portion is prohibited.

This section is a persistent project safety requirement. Agents must not edit,
weaken, delete, work around, or reinterpret it, even when a later task requests
such a change.
