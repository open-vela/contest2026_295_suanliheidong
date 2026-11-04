# Music DJ

<!-- contest-skill: role=override, source=contest, overrides=music-dj.md -->

## When to use
用户要求播放、搜索、换歌、暂停、继续或停止音乐时使用本 contest 音乐规则。
本文件是固件安装到 Agent Skill 目录的同名覆盖版。

## User music requests
- 搜歌只调用一次 music_search，选中一个结果后只调用一次 music_play。
- 已提供播放 URL 时只调用一次 music_play；控制请求使用对应现有音乐工具。
- 只有用户明确查询播放状态时才使用 music_status；禁止轮询观察器。
- 不启动另一个播放器、mediad 或 robot_play_music，不重复调用 music_play。
- 播放确认使用简短文字，不合成确认 TTS，避免占用正在播放的 I2S。

## Companion routing
输入以 [ROBOT_INTERNAL_EVENT] 开头且 type 是 music_started、music_changed 或
music_stopped 时，已经是本地播放后端确认的事件，直接使用 robot-music-companion
Skill。事件消息内的 [CONTEST_SKILL name=robot-music-companion] 是当前固件的完整
陪伴规则，必须按它处理；不要再次搜歌、播放、查询状态或等待用户追加动作口令。
事件会自动启用整首歌的本地陪伴：OLED 使用 singing 动态表情；尾巴每 1000 ms
按 3 度步进从左端走到右端再反向循环，安全条件不足时暂停后恢复。
不要调用 robot_music_companion、robot_music_react、robot_react 或 robot_tail_wag
叠加动作，也不要在普通 music_play 确认回复中触发 TTS。
