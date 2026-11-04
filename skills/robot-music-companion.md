# Robot Music Companion

<!-- contest-skill: role=event, source=contest, runtime=music-confirmation -->

## When to use
收到 [ROBOT_INTERNAL_EVENT] 且 type=music_started、music_changed、music_stopped 时，
立即使用本 contest Skill。source=music-dma 表示本地 DMA 已实际播放过 PCM。
本规则随事件完整提供，官方 music-dj 的播放建议不能替代本事件的陪伴行为。
session_id、event_id 是运行时令牌，只能使用当前事件给定的值。

## music_started / music_changed
播放确认事件会自动启用整首歌曲的本地陪伴策略，无需调用任何动作工具。
OLED 使用 `singing` 动态表情资源持续播放动画；高优先级语音或系统表情可以临时覆盖，
音乐表情会在覆盖结束后恢复。

普通歌曲的尾巴由本地 motion worker 驱动：每 1000 ms 移动 3 度，先从当前位置逐步到
左侧安全端点，再逐步到右侧安全端点，随后反向循环。端点限制为中心的 +/-45 度，
每次只使用尾巴舵机并在步进之间释放 PWM。

每个步进都会重新检查播放会话、实际音频输出、电源风险、对话、TTS 和动作占用。
条件不满足时跳过该步，播放恢复后本地继续；不重新请求大模型，也不根据歌曲节拍作出未经测量的声明。
助眠、冥想或摇篮曲事件可关闭尾巴，但仍保留 listening/singing 表情策略。

不要调用 `robot_music_react`、通用 `robot_react` 或 `robot_tail_wag` 叠加一次性动作，
也不要循环调用 `robot_music_companion`。事件本身已经是播放确认，后续调度完全由本地运行时处理。

## music_stopped
运行时立即撤销陪伴许可、取消尾巴动作并清除音乐表情。
本事件不再调用任何工具，不播放额外 TTS；迟到事件不能影响新歌曲。

## Tool and output limits
禁止 music_search/music_play/music_status/music_pause/music_resume、robot_play_music、
第二播放器、轮询观察器、robot_move、四腿 gait 和姿态动作。
禁止说话、确认语音、TTS 或长文本；完成工具后留空回复即可。
