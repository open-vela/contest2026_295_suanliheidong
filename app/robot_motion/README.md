# robot_motion

`robot_motion` is the single contest-local capability for the four validated
POWER_SAFE gaits: `forward`, `backward`, `left`, and `right`, plus the
serialized `robot_tail_wag` Agent Tool.

Both `robotctl` and the Agent `robot_move` Tool use this module. The Agent
Tool submits one request and waits for the worker to finish before returning
`completed=true`; a second request is rejected with `-EBUSY`. The legacy
`robotctl` commands keep their synchronous behavior by waiting for completion.

The `robot_tail_wag` Tool accepts bounded `cycles`, `amplitude_deg`, and
`period_ms` values. It uses the same motion worker and PWM ownership as
`robot_move`, returns the tail to its calibrated center, and does not overlap
with a leg gait.

Agent requests are limited to one gait step. The wait timeout is derived from
the requested period plus a fixed home/settle margin and is capped at two
minutes. If a wait reaches its deadline, the worker receives a cooperative
cancel request, stops PWM, and completes before the Tool returns its timeout.

`period_ms` controls gait speed: a lower value is faster and a higher value is
slower. The current POWER_SAFE profile uses 12--24 ms between 3-degree PWM
frames and 80--150 ms settling, while the home sequence keeps its conservative
timing.

Each gait phase submits its two listed leg servos in order, so only one servo
has a nonzero PWM duty at a time. Locomotion HOME preparation/finalization
intentionally excludes the tail; `robotctl home` remains the explicit command
that restores all five servos. Known no-op targets are skipped and reported in
the per-motion `writes`/`skipped` summary.

The Agent Tool accepts only semantic directions and bounded `steps` and
`period_ms` values. It never accepts servo names or angles. Valid physical
requests enter `robot_action_guard` before submission and leave it whether the
submission succeeds or fails.
