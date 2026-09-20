# robot_core

`robot_action_guard` protects physical side-effect Tools from the official
AI Agent response cache. It has no dependency on OLED, voice, motion, or
behavior implementations.

Use this sequence in every Tool that changes real robot state:

```c
/* Parse and validate input before entering the guard. */
robot_action_guard_begin(ROBOT_ACTION_MOTION, "robot_xxx");
ret = robot_xxx_execute(...);
robot_action_guard_end(ROBOT_ACTION_MOTION, "robot_xxx", ret);
```

The guard is fail-open: cache invalidation is synchronous at the physical
action boundaries and never blocks the physical action on a background worker.
Query-only Tools do not use the guard.

Side-effect Tools include expression, motion, posture, behavior, audio, and
system commands such as stop. Query Tools such as state and battery reads do
not need it.
