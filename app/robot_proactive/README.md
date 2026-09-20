# robot_proactive

Contest-local proactive runtime for the robot dog.

The runtime keeps a small fixed event queue and one worker.  Event sources
write facts into `robot_world_state`, then submit a typed event through the
policy layer.  Accepted events use the official
`message_bus_push_inbound()` API. The Agent receives the event on
`channel="voice"` with `chat_id="robot_proactive"`, so its normal ReAct,
Skill, Tool, OLED, motion, and TTS paths remain responsible for the response.

The first non-idle source is the music context interface:

```text
robot_proactive_music_update(state, track, category, mood, tempo_bpm)
```

It accepts observations only and produces `music_started`, `music_changed`,
or `music_stopped`. There is no contest-local music player in the current
tree. The voice app observes the official `music_status` Tool API at a low
rate and reports only confirmed PLAYING/STOPPED transitions.

The official media framework exposes callbacks only on a caller-owned player
handle. The official `music_play` handle and state are private to
`packages/ai_agent/src/tools/tool_media.c`, so no contest-side callback can be
attached without an upstream API addition. PAUSED is retained as state-only;
the observer does not wake the Agent for pause/resume. `robot_agentctl music`
is a debug/test injection only; it does not control the official player.

The policy applies global and per-event cooldowns, event-id deduplication,
freshness checks, and conversation/motion/TTS busy suppression before waking
the official Agent. Music events observed while conversation, motion, or TTS
is active are coalesced as the latest pending event and dispatched when the
robot becomes idle. Weather, Feishu, and meal event types are reserved in the
schema for later sources and are not fabricated by this build.

Diagnostic commands:

```text
robot_agentctl status
robot_agentctl on
robot_agentctl off
robot_agentctl activity
robot_agentctl trigger test
robot_agentctl trigger idle
robot_agentctl music start [track category mood tempo]
robot_agentctl music change [track category mood tempo]
robot_agentctl music stop
```

The default idle timeout and global cooldown are both 60 seconds.  The Skill
source is `contest2026_295_suanliheidong/skills/robot-proactive-idle.md`.
The firmware installer derives the runtime destination from the official
`AGENT_SKILLS_DIR` configuration.  The host-side script accepts that same
directory as its first argument.
