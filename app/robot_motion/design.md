# Robot Motion and Music Companion Feature Design

## 1. Design Scope

This change stays inside the contest-local application. It preserves the
existing playback backend, event queue, PWM ownership, motion worker, and
internal music message behavior.

## 2. Tail Reset Timing

`robotctl_servo_move_one()` remains the single low-level stepping primitive.
Its existing safe step and ramp/settle logic are unchanged. Explicit tail
commands and every tail-to-center reset now pass the existing 30 ms limb
interval. The low-level tail exception remains so the three-degree music
micro-step can retain its established cadence. `robot_motion_home_power_safe()`
continues to pass the gait-derived interval and therefore resets all five
servos with the same slow floor.

Music tail steps retain their separate local policy: three-degree segments are
submitted every 1000 ms. The low-level floor may add at most the existing
minimum frame delay; the period scheduler remains the authority for the music
cadence. Any cooldown arithmetic that accounts for a per-frame delay uses the
same named interval function instead of a stale literal.

## 3. Music Skill and Companion Authorization

The DMA confirmation worker marks a session as `announced` but leaves
`companion_enabled` false and its event token unset. It still posts the
confirmed `music_started`/`music_changed` event through
`robot_proactive_music_session_update()`.

`robot_proactive_send_to_agent()` keeps embedding
`robot-music-companion.md`, setting `AGENT_MSG_FLAG_INTERNAL_MUSIC`, and arming
the world-state event token. After `message_bus_push_inbound()` succeeds and
without holding the proactive or music mutex, it calls
`robot_music_media_companion_start(session_id, event_id, false, true)`.
The call is idempotent for retries and validates the active session, playing
state, and exact event token. A failed message push rolls back the armed token;
the existing pending-event queue retries the event. A failed companion start
is returned as an event failure so the pending queue retries it without
starting periodic tail motion early.

Music stop continues to clear the media companion state and expression before
publishing the stop event. Session/event checks in `robot_music_media_motion_allowed()`
and `robot_world_state_claim_music_tail_repeat()` reject stale tail requests.

## 4. Data Flow

```text
DMA progress > 0
  -> robot_proactive_music_session_update()
  -> robot_proactive_process()
  -> embed and enqueue robot-music-companion event
  -> arm world session/event token
  -> companion_start(session,event,false,true)
  -> local companion tick every 1000 ms
  -> claim exact session/event
  -> robot_motion music tail worker
```

The Agent loop may consume the internal event without LLM/tool execution. That
is intentional: local media code performs the actual authorization and tail
motion, while the event still records and carries the contest Skill rules.

## 5. Error and Concurrency Rules

- No companion start is called while `g_music_lock` or `g_proactive.lock` is held.
- Failed enqueue or authorization leaves no active tail permission.
- Duplicate event IDs are filtered by world state before enqueue.
- A stop or newer session clears the companion flag and invalidates old tokens.
- PWM is stopped after every bounded servo move, including cancellation and
  error paths.

## 6. Traceability Matrix

| Requirement ID | Requirement | Design element | Verification |
| --- | --- | --- | --- |
| R1 | Tail reset uses the limb slow floor | Tail reset callers pass `robot_motion_limb_interval()` (30 ms floor) | Source inspection and host assertions |
| R2 | Tail reset keeps safe stepping and stop semantics | Existing 3-degree loop, cancellation checks, settle, STOP/close paths | Checkpatch and focused test build |
| R3 | Music event is created only after DMA confirmation | `robot_music_confirm_worker()` keeps `completed != 0` gate | Existing progress path plus source inspection |
| R4 | Skill is embedded and marked internal | `robot_proactive_send_to_agent()` resource injection and flag | String/flag grep and build |
| R5 | Tail starts only after successful event delivery | post-enqueue `robot_music_media_companion_start()` | Focused host test and source inspection |
| R6 | Duplicate and stale events cannot move the tail | world-state event/session validation and claim gates | Existing event tests plus build |
| R7 | Playback/TTS/I2S behavior remains unchanged | No edits to playback transport or audio data path | Diff scope and contest build |

## 7. Non-Goals

No changes are made to music search, HTTP transport, TTS, I2S, board PWM
calibration, or shared upstream Agent sources.
