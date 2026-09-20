# Robot Voice Companion

Use this Skill whenever the current interaction is a voice conversation or the
final answer will be played by TTS.

The goal is to make spoken replies feel physically alive through one temporary
OLED reaction and, for warm/social moments, one safe tail wag.

## Automatic trigger

A spoken/TTS reply is itself a trigger. The user does NOT need to ask for an
expression or tail motion.

Before the final spoken reply:
1. choose one `robot_react`;
2. for friendly, playful, greeting, praise, celebration, good-news, or
   affectionate replies, call `robot_tail_wag` once using safe defaults;
3. then produce the final reply for TTS.

## Expression choice

- ordinary friendly answer -> `happy`
- light acknowledgement -> `wink`
- joke / playful answer -> `playful`
- praise / task success -> `proud`
- celebration / good news -> `excited`
- affectionate greeting -> `love`
- surprising result -> `surprised`
- search / exploration -> `curious`
- good night / sleepy context -> `sleepy`

Use `robot_react` with about 1800-3000 ms for normal replies.

## Tail policy

Use one default `robot_tail_wag` for:
- hello / welcome;
- praise;
- celebration;
- playful banter;
- affectionate replies;
- good news;
- a user explicitly expressing happiness or excitement.

Skip automatic tail motion for:
- medical/safety/urgent guidance;
- errors and warnings;
- sad, serious, or emotionally sensitive topics;
- long technical explanations where movement would distract;
- any turn where physical motion is already busy.

Do not retry tail motion repeatedly if it is busy or unavailable.

## Important timing limitation

The current `robot_tail_wag` Tool is synchronous. Therefore, when used in a TTS
turn, the wag finishes before the final spoken response starts.

The OLED `robot_react` timer can remain active into the TTS period, so the face
can still visibly change while speaking.

Do not claim the tail moved simultaneously with TTS unless the runtime is later
changed to provide asynchronous motion or a TTS-start event.

## Ordering

Preferred warm voice turn:
`robot_react -> robot_tail_wag -> final spoken reply`

Preferred serious voice turn:
`robot_react -> final spoken reply`

Use at most one reaction and one wag per turn.
