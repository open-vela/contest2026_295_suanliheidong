# Robot Motion and Music Companion Incremental Requirements

## 1. 现状分析

目标功能位于 contest-local `app/robot_motion/`、`app/robotctl/`、
`app/robot_proactive/` 和 `app/robot_voice/`，采用串行运动 worker、PWM
舵机控制、音乐 DMA 确认事件和 contest Skill 资源注入架构。

现有文件与职责：

| 文件 | 当前职责 |
| --- | --- |
| `app/robotctl/robotctl_servo.c` | PWM 打开、角度换算、3 度安全步进、单舵机移动和停止 |
| `app/robotctl/robotctl_main.c` | `robotctl home`、单舵机命令和四肢运动命令入口 |
| `app/robot_motion/robot_motion.c` | 四肢 gait、home 复位、尾巴动作和音乐尾巴 worker |
| `app/robot_motion/robot_motion.h` | 运动参数边界和公开接口 |
| `app/robot_proactive/robot_proactive.c` | 音乐事件队列、Skill 消息注入和事件去重 |
| `app/robot_voice/robot_music_media.c` | 音乐 DMA 播放确认、陪伴状态和周期尾巴请求 |
| `skills/robot-music-companion.md` | 音乐事件的独占陪伴策略 |

当前四肢运动使用 `robot_motion_limb_interval()`，最低间隔为 30 ms。
尾巴的低层移动允许绕过该最低间隔，单舵机命令使用 20 ms；音乐确认后
也会先把陪伴状态置为启用，再异步发送 Skill 事件，运行时没有显式记录
“该音乐事件已经完成 Skill 触发并授权尾巴”的边界。

## 2. 新功能需求

### 2.1 尾巴复位速度

1. 尾巴舵机返回校准中心或执行显式单舵机复位时，必须使用与四肢相同的
   慢速 PWM 步进下限（当前为 30 ms）。
2. 复位仍使用现有 3 度安全步进、取消检查、PWM 停止和 settle 行为。
3. 不改变尾巴动作的角度边界、中心角（85 度）或音乐陪伴的 1 度周期策略。
4. 不能通过一次性大角度 PWM 写入跳过步进，也不能影响四肢 gait 的时序。

### 2.2 音乐陪伴 Skill 与尾巴绑定

1. 只有实际 DMA 播放确认后，才产生 `music_started` 或 `music_changed`
   事件并注入 `robot-music-companion` Skill。
2. 每个已接受的音乐事件必须在本地运行时显式调用一次陪伴授权，授权成功
   后才允许周期尾巴 worker 提交尾巴动作；不得依赖 Agent 解析文本来驱动
   尾巴。
3. 同一 session/event 不得重复授权或重复触发；迟到、旧 session 和停止
   事件不能影响新歌曲。
4. 音乐停止时撤销陪伴授权、清除音乐表情并取消后续尾巴动作。
5. 播放器、网络请求、TTS/I2S 数据路径和 `music-dj` 播放能力保持不变。
6. 音乐陪伴事件仍必须携带 contest-local `robot-music-companion.md` 内容，
   并由 `AGENT_MSG_FLAG_INTERNAL_MUSIC` 标记为内部事件，禁止触发普通用户
   工具链、二次播放器或 TTS。

## 3. 工作原理与架构接口

尾巴与四肢共享 `robotctl_servo_move_one()` 的 PWM、角度缓存和取消检查；
运动 worker 通过 `robot_motion_home_power_safe()` 执行复位，音乐 worker 通过
`robot_motion_music_tail_repeat_async()` 执行周期尾巴步进。

音乐链路为：

```text
DMA playback progress
  -> robot_proactive_music_session_update()
  -> robot-music-companion internal event + event id
  -> robot_music_media_companion_start()
  -> periodic tail request
  -> robot_motion worker -> robotctl_servo_move_one()
```

`robot_music_media_companion_start()` 是单次幂等授权点；周期动作仍由本地
播放确认、会话、电源、对话和 TTS 状态门控。停止事件通过现有
`robot_music_media_end_locked()` 撤销授权。

## 4. 影响范围

需要修改：

- `app/robotctl/robotctl_servo.c`：统一尾巴与四肢的最低步进间隔。
- `app/robotctl/robotctl_main.c`：单舵机尾巴命令使用四肢同一速度计算。
- `app/robot_motion/robot_motion.c`：保持尾巴复位路径使用统一慢速间隔，
  并更新音乐尾巴时序计算中的固定步进假设。
- `app/robot_voice/robot_music_media.c`：DMA 确认后等待对应 Skill 事件完成
  本地陪伴授权，避免未触发 Skill 就启动尾巴。
- `app/robot_proactive/robot_proactive.c`：成功投递音乐 Skill 事件后显式
  调用本地陪伴授权，并保持事件去重和失败重试。

不新增文件，不修改共享 `nuttx/`、上游 Agent 包、播放后端和板级配置。

## 5. 实现约束

- 所有修改限定在 `contest2026_295_suanliheidong/**`。
- 保留现有工作树改动和未跟踪文件，不执行清理、重置或全量暂存。
- 代码注释使用英文；文档可使用中文。
- 事件授权不得在持有运动锁时调用会再次获取世界状态或音乐锁的函数。
- 任何错误路径都必须释放 PWM、清理事件 payload，并让旧 session 失效。
- 构建通过只证明编译、链接和镜像生成；尾巴速度和实际 Skill 触发仍需板端日志确认。

## 6. 性能指标与验证目标

| 指标 | 目标 |
| --- | --- |
| 四肢/尾巴复位最小 PWM 步进间隔 | 30 ms，允许已有 ramp 额外延时 |
| 尾巴安全步进 | 3 度（现有低层约束） |
| 音乐尾巴周期步进 | 3 度 / 1000 ms，保持现有策略 |
| 音乐 Skill 触发 | 每个有效 DMA session 的 started/changed 事件最多一次 |
| 停止后的尾巴动作 | 0 个新授权动作 |
| 普通 Agent/TTS 工具副作用 | 0 |

## 7. 功能 Checklist

- [ ] 尾巴显式单舵机复位使用四肢相同的 30 ms 最小间隔
- [ ] `robotctl home` 的尾巴复位保持 3 度步进和取消/停止语义
- [ ] 音乐 DMA 确认事件包含并投递 `robot-music-companion` Skill
- [ ] Skill 事件成功投递后才启用本地周期尾巴
- [ ] 音乐事件重复、旧 session、停止后的动作被拒绝或丢弃
- [ ] 播放器、TTS、I2S 和四肢 gait 行为无回归
- [ ] 运行 checkpatch、主机语法/单测检查和 contest 固件构建
