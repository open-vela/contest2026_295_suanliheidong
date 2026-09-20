# robot_voice

Independent contest-board MiMo voice application. It does not include or
modify `packages/ai_agent` voice code. The application uses the original
public `vela_https_request()` transport ABI and contest-owned capture,
protocol, WAV/Base64, and I2S playback modules. The normal voice path sends
ASR text through the existing ai_agent message bus, so the original LLM,
tools, history, and outbound voice dispatch remain the single shared path.

## Commands

```text
robot_voice set_key <api_key>
robot_voice test_capture
robot_voice test_asr
robot_voice test_llm <text>
robot_voice test_tts <text>
robot_voice once
robot_voice diag
```

`set_key` writes the same config key used by `set_llm`; normally configure the
key with `vela> set_llm ... <api_key>` instead. Capture is 16 kHz, mono,
signed PCM16 for three seconds. MiMo TTS is requested as 24 kHz mono PCM16
WAV; playback duplicates each source sample into left and right I2S slots at
24 kHz.

For the shared Vela voice ABI, select `mimo-v2.5-asr` and
`mimo-v2.5-tts`, then use `voice_start`/`voice_stop` for continuous capture.
The `robot_voice once` diagnostic performs one capture and publishes the ASR
text to the same bus; it does not create a second LLM agent.
