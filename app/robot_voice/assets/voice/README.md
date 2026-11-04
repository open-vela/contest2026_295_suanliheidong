# 本地提示音输入目录

请将下列 WAV 放在当前目录（项目中的 `app/robot_voice/assets/voice/`）：

| 文件名 | 请录制或合成的文本 | 触发时机 |
|---|---|---|
| `startup.wav` | 你好，我是 OpenVela，我们开始聊天吧，请用‘你好，openvela’唤醒我。 | `voice_start` 后麦克风准备就绪，播放一次 |
| `network_error.wav` | 网络似乎有些异常，请稍后再试。 | ASR/LLM/TTS 重试失败，或等待回复超时 |
| `chat_exit.wav` | 我先休息啦，需要时再叫我。 | 连续 30 秒无输入，或用户说“结束聊天/退出聊天” |
| `not_heard.wav` | 没听清，请再说一次。 | 录音/文本超限等无法提交完整请求的情况 |

要求：**24 kHz、单声道、16-bit PCM、普通 WAV，每段大于 0 且不超过 8 秒**。
这与麦克风的 16 kHz 输入不同；本地提示走现有 24 kHz 扬声器播放通路。
建议使用自然、简短的语速，开头与末尾少留静音，不要削波。
程序沿用现有 TTS 的半幅度播放增益，请在板卡上试听音量。

Make 和 CMake 编译时自动检查文件并把 PCM 编进只读资源，随后随固件烧录。
运行时无需复制到 `/etc`，也不用挂载 ROMFS。每次更换音频后需重新编译和烧录。
目录里目前没有伪造的语音文件；缺少某段时，该事件会播放短提示音，并输出缺失文件名。
文件格式错误或名字拼错会让资源生成失败，避免把错误音频烧进去。

在 `app/robot_voice/` 下可先校验全部文件：

```bash
python3 scripts/generate_voice_prompts.py --input assets/voice \
  --output .generated/voice_prompt_check.c --require-all
```

板卡命令：

```text
robot_voice prompt_list
robot_voice prompt_test startup
robot_voice prompt_test network_error
robot_voice prompt_test chat_exit
robot_voice prompt_test not_heard
```

这几个命令不需要在线 TTS 或 API key。电机运行时仍遵守现有播放保护。
“开始的问候”指 `voice_start`，不在每次唤醒后再播一次，避免盖住用户的第一句话。
