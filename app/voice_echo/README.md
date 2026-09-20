# voice_echo

On-demand raw PCM record-then-playback test application for the contest board.

```text
nsh> voice_echo
nsh> voice_echo --once
nsh> voice_echo recordmem
vela> voice_echo recordmem
```

The default and `--once` commands immediately record exactly five seconds from
I2S0 RX, play the captured PCM once through I2S1 TX, and return to NSH.
`recordmem` records at most three seconds and enters ASR directly. GPIO14 is not connected
on the current contest board, so this application does not use GPIO14, GPIO0,
or `/dev/buttons`.

OLED state transitions are best-effort. Playback uses `REC 5 SEC` -> `PLAY` ->
`DONE`; `recordmem` uses `REC 3 SEC` -> `ASR REQ` -> `ASR DONE`.
OLED failures are reported but do not abort audio.

Audio profile: 16 kHz input, 24 kHz output, mono, 16-bit samples in 16-bit
slots. The rates come from `eda-robot-pro/config.h`; sample width, slot width,
and external module behavior are provisional and not hardware-validated.
The record buffer is derived as `16000 * 5 * sizeof(int16_t)` (160000 bytes).
Playback performs the existing 16 kHz to 24 kHz 3:2 conversion in DMA-sized
chunks, so it does not allocate a second complete playback buffer.

`recordmem` is the board-side voice-pipeline stage. It records at most three
seconds of the same 16 kHz mono PCM, builds a standard 44-byte WAV header in
RAM, and immediately submits the PCM buffer to the registered ASR backend. No
path argument, filesystem, `fopen`, or `fwrite` is used. The WAV header is kept
as an in-memory view for the next audio stage; the ASR interface receives the
PCM portion because its contract is 16-bit LE, 16 kHz, mono PCM.

When `CONFIG_EXAMPLES_AI_AGENT_VELA` is enabled, the `vela>` CLI exposes the
same `voice_echo recordmem` command and calls the application in-process. It
therefore reuses the ai_agent voice backend and credentials configured in
`vela>`. The default backend is MiMo; configure its key with
`set_mimo_key <api_key>` or `set_llm ... <api_key>`, then the command performs
ASR, pushes the recognized text to the common agent message bus, and lets the
agent produce the LLM/TTS response through the normal voice channel. The
standalone `nsh>` command still reports `ASR unavailable` when ai_agent is
disabled.
