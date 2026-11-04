# Expressive Companion Router

<!-- contest-skill: role=router, source=contest, official-fallback=none -->

This is a routing policy for contest-local companion behavior. It prevents
generic expression rules from competing with the more specific skills.

## Precedence

1. `[ROBOT_INTERNAL_EVENT]` with `type=music_started`, `music_changed`, or
   `music_stopped` uses `robot-music-companion` exclusively.
2. A voice reply that will be spoken uses `robot-voice-companion`.
3. Idle events use `robot-proactive-idle`.
4. Only other social interactions may use the generic rules below.

Never add a second music or tail action when a more specific skill already
handled the event. Do not call motion tools in a loop, and do not invent servo
angles; the motion tool owns its power-safe defaults.

For an unmatched short social reply, one temporary `robot_react` is enough.
Use at most one default `robot_tail_wag` for a clearly warm or celebratory
interaction, and skip movement for errors, safety topics, serious replies, or
when another motion is active. Do not use `robot_set_expression` unless the
user explicitly requests a persistent expression.
