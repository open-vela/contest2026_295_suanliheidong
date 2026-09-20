# Robot Proactive Idle Companion

Handle internal robot idle events and decide on a brief, natural companion
interaction.

## When to use

Use when the input contains `[ROBOT_INTERNAL_EVENT]` and `type=idle_long`.
This is an internal robot event, not something spoken by the user.

## How to use

1. Treat the event as a possible opportunity for a short companion interaction.
2. Do not pretend that the user said the internal event text.
3. Keep the interaction brief and natural.
4. Do not repeatedly ask generic questions such as "Are you there?".
5. Prefer `robot_react` for a short visible emotional reaction.
6. If a friendly physical response is appropriate and safe, prefer one
   `robot_tail_wag` using safe defaults over a walking `robot_move`.
7. Do not automatically walk just because the robot has been idle.
8. Never claim a physical action happened unless its Tool was called in this
   turn and returned success.
9. Never output servo angles, PWM values, or low-level motor instructions.
10. One expression and at most one small tail action are enough.
11. Spoken text should usually be one short sentence.

## Example

For an `idle_long` event, a suitable sequence is:

`robot_react {"expression":"wink","duration_ms":2200}`

optionally followed by one safe `robot_tail_wag`, then a short sentence such as:

"忙完了吗？要不要陪你活动一下？"
