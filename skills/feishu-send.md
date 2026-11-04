# Feishu Send

<!-- contest-skill: role=workflow, source=contest, official-fallback=none -->

可靠地完成“先执行主任务、再把真实结果发送到飞书群”的复合请求。只要用户要求
向飞书发送消息，就必须先完成前置查询或动作并确认工具成功，再发送结果。

## When to use

Use this Skill when the user asks to把内容发到飞书、发给飞书群、告诉群里，
或在语音中要求把查询结果、动作结果或一句话发送到飞书。

## Required procedure

1. Identify the primary task before the send request. The send operation is the final step,
   not a substitute for the primary task.
2. Complete the primary task with its real tool and keep its returned result:
   - 时间：调用 `get_current_time`，不要凭记忆填写时间；
   - 天气：调用 `get_weather`，先得到指定地点的实际天气；
   - 播放音乐：先按 `music_search` → `music_play` 找到并启动歌曲，再使用实际歌曲名和歌手；
   - 其他查询或动作：先调用对应工具，并只使用工具返回的结果。
3. If the primary task fails, do not send a success message. Report the primary failure and
   ask whether the user wants the failure details sent to Feishu.
4. After the primary task succeeds, call `feishu_send_text` with the actual result. 没有指定群 ID 时省略 `chat_id`，让工具
   使用最近收到的有效飞书群消息作为目标；用户明确给出 `oc_...` 时才传入 `chat_id`。
5. 等待发送工具返回结果。只有发送返回成功，才能说“已发送到飞书群”。
6. 如果发送工具失败，明确说“发送失败”，保留失败原因。不要把准备发送、LLM 回复或
   本地 TTS 成功当成飞书发送成功。
7. 如果工具报告没有已知群聊，要求用户先在目标群 @机器人发一条消息，或提供群聊
   `chat_id`，然后重试。

## Tool selection

- 普通群消息：`feishu_send_text`
- 用户明确要求 @某人：使用 `feishu_chat_members` 后再使用 `feishu_send_mention`
- 不要用 `feishu_send_mention` 代替普通消息，也不要重复调用多个发送工具。

## Examples

用户：“请把这个时间发到飞书里。”

→ `get_current_time {}`
→ `feishu_send_text {"text":"<工具返回的当前时间>"}`
→ 仅在第二步成功后回复：“当前时间是……，已发送到飞书群。”

用户：“把北京今天的天气发到飞书。”

→ `get_weather {"location":"北京"}`
→ `feishu_send_text {"text":"北京今天的天气：<工具返回结果>"}`
→ 只有两个工具都成功才确认已发送。

用户：“播放一首歌并把歌曲信息发送到飞书。”

→ `music_search {"keyword":"热门歌曲"}`
→ 选择一个实际结果后调用 `music_play`，等待播放启动成功
→ `feishu_send_text {"text":"正在播放：<实际歌曲名> - <实际歌手>"}`
→ 如果搜索或播放失败，不得声称歌曲已播放或消息已发送。
