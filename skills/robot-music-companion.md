# Robot Music Companion

Handle structured music context events from the robot proactive runtime and
turn them into visible, physical companion behavior.

## When to use

Use when the input contains `[ROBOT_INTERNAL_EVENT]` with:
- `type=music_started`
- `type=music_changed`
- `type=music_stopped`

These fields are observations from the robot's music source, not user speech.

## Strong trigger rule

A `music_started` event is itself a request for proactive companion behavior.

Do NOT wait for the user to additionally say:
- "摇尾巴"
- "换表情"
- "开心一点"
- "跳舞"

When music is confirmed playing, the default behavior is:
1. choose and call one `robot_react`;
2. when safe and the mood is not explicitly calm/sleepy, call
   `robot_tail_wag` once using safe defaults;
3. normally do not speak, so TTS does not interrupt the song.

## music_started

For `type=music_started`:

1. Read only supplied facts such as track, category, mood, and tempo.
2. Call `robot_react`.
3. Unless the supplied context is calm, sleep, meditation, lullaby, or a
   physical motion is already busy, call `robot_tail_wag` once.
4. Do not call another music-control Tool; playback already started.

Reaction selection:
- energetic / fast / upbeat -> `excited` or `cool`;
- happy / pop / ordinary unknown mood -> `singing` or `happy`;
- playful -> `playful`;
- calm / sleep / lullaby -> `sleepy`.

If mood/tempo is unknown, default to `singing` plus one safe tail wag.

## music_changed

For a meaningful `type=music_changed`:
- update the face with one `robot_react`;
- one safe tail wag is allowed when the new track is energetic or playful;
- do not wag repeatedly for metadata noise or duplicate events.

## music_stopped

For `type=music_stopped`:
- do not call `robot_tail_wag`;
- a short `happy` reaction is optional;
- do not claim playback continues.

## Tool policy

Use:
- `robot_react` for temporary visible music reactions;
- `robot_tail_wag` for one bounded safe physical response.

Prefer `robot_react` over persistent `robot_set_expression` for automatic music
personality.

Do not use `robot_move` for ordinary music. Walking while music plays is not a
default behavior.

For `robot_tail_wag`, prefer no optional numeric arguments so the current
power-safe defaults are used. Never expose or invent servo angles, PWM values,
or low-level motor instructions.

## Output policy

A music context event usually needs no spoken sentence at all.

Prefer:
`robot_react -> robot_tail_wag`

over:
`robot_react -> long TTS response`

Never call `music_play`, `music_pause`, `music_resume`, or another
music-control Tool in response to the internal event; the event already came
from the official player.
