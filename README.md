# 基于 openvela 的 AI 智能机器狗

## 一、项目简介

VoicePet 是一款基于 openvela、ESP32-S3 N16R8 和 MiMo 服务构建的具身智能机器狗。系统将语音唤醒、语音识别、大模型 Agent、机器人动作、OLED 表情、音乐播放、语音播报和飞书群聊连接成完整闭环。

机器狗既可以通过语音接收指令，也可以通过飞书群聊中的 @aidog 接收指令，并调用实际的运动、表情、音乐和网络能力。

主要能力包括：

- 使用 openvela 官方 ai_agent 统一处理文本、语音和飞书消息；
- 使用 TensorFlow 训练语音唤醒模型，并转换为 TensorFlow Lite Micro 模型运行在板端；
- 使用 MiMo v2.5 ASR 进行语音识别；
- 使用 MiMo v2.6 Flash 进行大模型推理；
- 启用 MiMo v2.6 Flash 流式输出，降低首句回复延迟；
- 使用 MiMo v2.5 TTS 进行语音播报；
- 支持连续语音对话和唤醒词触发；
- 支持飞书机器人应用、群聊 @aidog 指令和主动进行结果回复及播报；
- 支持语音查询信息后，将实际结果发送到飞书群；
- 支持前进、后退、左转、右转、摇尾巴和 OLED 表情；
- 播放音乐时可以通过音乐事件触发机器狗自主摇尾巴；
- 支持本地 WAV、openvela Media 和在线音乐播放；
- 支持开机自动初始化 Agent、网络、模型、飞书和语音监听；
- 在作品侧增加音频缓冲、播放调度和动作保护逻辑，不修改官方 WLAN 驱动源码。

## 二、版本更新概览

### 2.1 语音唤醒模型

系统通过采集语音样本训练 KWS（Keyword Spotting，关键词唤醒）模型：

1. 使用 TensorFlow 训练语音唤醒模型；
2. 将模型转换为 INT8 TensorFlow Lite Micro 模型；
3. 在 ESP32-S3 上使用 TensorFlow Lite Micro 推理；
4. 板端使用与训练验证一致的音频前处理参数；
5. 通过唤醒词检测、RMS 门限、分数平均、迟滞和冷却时间降低误触发。

当前板端音频前处理使用：

- 16 kHz；
- 单声道；
- PCM16；
- 2 秒分析窗口；
- 1600 个采样点推理步长（0.1 秒 hop）；
- 400 个采样点分析帧长（25 ms）；
- 160 个采样点帧移（10 ms）；
- 512 点 FFT；
- 40 个 Mel 频带；
- 周期 Hann 窗；
- 功率谱、Mel 滤波、对数或 dB 归一化；
- 输入张量为 `[1, 48, 40, 1]`，数据类型为 INT8；
- 模型算子为 `FULLY_CONNECTED`、`RESHAPE` 和 `SOFTMAX`；
- 模型导入记录的阈值为 `0.99`；
- golden 自检输出为 `[84, -84]`，表示二分类输出的 INT8 原始值，不是概率值。

模型输出使用 INT8 量化参数进行解量化，正类为输出索引 1。板端当前还会在模型分数上执行 RMS 门控、3 窗口滑动平均、连续 2 次确认、迟滞和冷却处理；当前固件的覆盖触发阈值为 `0.8`，因此它与模型导入记录的 `0.99` 不同。实际触发行为应以固件配置和运行日志为准。

当前唤醒词可以配置为“你好，openvela”。

### 2.2 MiMo 模型升级

当前模型配置如下：

| 功能 | 模型 |
|---|---|
| ASR | mimo-v2.5-asr |
| LLM | mimo-v2.6-flash |
| TTS | mimo-v2.5-tts |

MiMo v2.6 Flash 使用流式响应。Agent 可以在 Flash 仍然输出时开始处理完整句子和 TTS，从而减少首句回复等待时间。

### 2.3 飞书机器人

系统已经支持飞书机器人应用：

- 在飞书开放平台创建机器人应用；
- 启用机器人和群聊相关权限；
- 使用事件流连接飞书服务；
- 将机器人添加到群聊；
- 在群内通过 @aidog 发送指令；
- Agent 调用动作、查询、音乐或其他工具；
- 将实际执行结果回复到飞书群。

也支持通过语音发送飞书消息。例如：

~~~text
查询北京今天的天气，并发送到飞书。
播放一首歌，并把歌曲信息发送到飞书。
请把当前时间发送到飞书群。
~~~

对于组合指令，系统应先完成主任务，再发送实际结果。例如“查询北京天气并发送到飞书”会先调用天气服务，再将天气结果发送到飞书。

### 2.4 表情与音乐陪伴

系统增加了表情状态管理和主动陪伴逻辑：

- Agent 可以设置持续表情；
- Agent 可以触发短时情绪反应；
- OLED 显示根据语音、动作和音乐状态更新；
- 音乐开始播放时产生 music_started 事件；
- 音乐陪伴 Skill 接收音乐事件；
- 机器狗可以在播放音乐时执行低幅度、低速尾巴动作；
- 动作由统一的运动 Worker 和保护逻辑执行。

音乐播放仍然保留 music_search 和 music_play 接口，同时由 contest 项目内的音乐观察器和音乐陪伴 Skill 增加机器狗行为。

### 2.5 开机自动启动

启用自动启动配置后，系统开机后可以自动完成：

1. 启动 ai_agent；
2. 连接 Wi-Fi；
3. 配置 LLM；
4. 配置 ASR；
5. 配置 TTS；
6. 配置飞书应用；
7. 启动语音监听。

自动启动失败时应按步骤重试，只有前一步成功后才进入下一步。调试时仍可通过 NSH 手动执行单步命令。

## 三、选题方向

**AI 硬件产品创新**。

本作品将大模型从纯软件对话扩展到真实机器人：

- Agent 负责理解意图和选择能力；
- Skill 负责组织领域行为；
- Tool 负责执行动作；
- 硬件负责运动、显示、采集和播报；
- 飞书提供远程群聊交互入口。

作品重点解决了嵌入式 AI 产品中的语音交互闭环、硬件安全、连续音频播放、网络服务接入和多模块协同问题。

## 四、功能与系统架构

### 4.1 语音交互链路

~~~text
INMP441 麦克风
    ↓ I2S0 RX
robot_voice_capture
    ↓
KWS 前处理与 TensorFlow Lite Micro 唤醒模型
    ↓ 唤醒后进入语音采集
MiMo v2.5 ASR
    ↓
openvela ai_agent
    ↓
MiMo v2.6 Flash 流式推理
    ↓ Skills / Tool Calls
MiMo v2.5 TTS
    ↓
robot_audio_playback
    ↓ I2S1 TX
MAX98357
    ↓
扬声器
~~~

麦克风输入为 16 kHz、单声道、PCM16。

语音输出为 24 kHz、单声道、PCM16。

文本输入、语音输入和飞书输入共用同一个 Agent、Skill、Tool 和结果分发链路。

### 4.2 飞书消息链路

~~~text
飞书群聊 @aidog
    ↓
Feishu Event Streaming / WebSocket
    ↓
Feishu 消息解析
    ↓
openvela ai_agent
    ↓
Skill / Tool
    ↓
机器人动作、查询、音乐或消息发送
    ↓
飞书群回复
~~~

语音指令也可以调用飞书发送工具：

~~~text
语音指令
    ↓
ASR
    ↓
Agent
    ↓
完成主任务
    ↓
发送实际结果到飞书群
~~~

### 4.3 常开语音和音乐策略

voice_start 开启后，系统进入语音监听状态。

当检测到有效唤醒词和语音指令时：

1. 保留当前语音会话；
2. 必要时暂停或停止音乐；
3. 将语音发送到 ASR；
4. 将识别结果交给 Agent；
5. 完成 Tool、TTS 或飞书操作；
6. 根据会话状态重新进入监听。

音乐播放期间，语音输入具有更高优先级，避免音乐和语音采集同时竞争音频资源。

### 4.4 Agent Tool

| Tool | 能力 | 主要实现 |
|---|---|---|
| robot_move | 前进、后退、左转、右转 | robot_motion_tool.c |
| robot_tail_wag | 安全摇尾巴 | robot_motion_tool.c |
| robot_set_expression | 设置持续表情 | robot_expression_tool.c |
| robot_react | 短时情绪反应 | robot_expression_tool.c |
| robot_play_music | 播放本地 WAV | robot_music_player.c |
| music_search | 在线搜索音乐 | openvela Tool |
| music_play | 在线播放音乐 | openvela Tool |
| feishu_send_text | 向飞书群发送结果 | feishu-send.md |
| get_weather | 查询天气 | openvela Tool |
| get_current_time | 查询当前时间 | openvela Tool |

运动使用统一 Worker、动作保护和舵机限速逻辑管理。LLM 只调用高层动作接口，不直接操作任意 PWM 或舵机角度。

## 五、Skill 组织

contest 项目内的 Skill 位于：

~~~text
contest2026_295_suanliheidong/skills/
~~~

主要 Skill 包括：

| Skill | 作用 |
|---|---|
| manifest.json | 定义 contest Skill、官方 Skill 和用户 Skill 的优先级 |
| expressive-companion.md | 表情和情绪行为路由 |
| robot-voice-companion.md | 语音陪伴和对话行为 |
| robot-music-companion.md | 音乐开始、结束和音乐陪伴行为 |
| robot-proactive-idle.md | 空闲状态下的主动行为 |
| robot-meal-companion.md | 饮食和陪伴场景 |
| feishu-send.md | 主任务完成后向飞书发送实际结果 |
| music-dj.md | 音乐搜索、播放和音乐行为扩展 |

contest Skill 优先于官方同名 Skill。没有覆盖的能力继续使用 openvela 官方 Skill。

## 六、目录结构

~~~text
contest2026_295_suanliheidong/
├── app/
│   ├── robot_voice/        # 唤醒、采集、ASR、LLM、TTS、音频播放和语音入口
│   ├── robot_motion/       # 步态、运动 Worker 和运动 Tool
│   ├── robot_expression/   # 表情状态、OLED 绘制和表情 Tool
│   ├── robotctl/           # 手工运动调试命令
│   ├── robot_core/         # 动作保护和网络适配
│   ├── robot_proactive/    # Skill 安装、事件和主动交互
│   └── robot_agentctl/     # Agent 辅助控制
├── board/contest_board/    # ESP32-S3 板级初始化、I2S、音频桥和引脚定义
├── skills/                 # contest 自定义 Skill
├── scripts/                # Skill 打包和构建脚本
├── docs/                   # 设计和调试文档
├── logs/                   # AI Coding 对话和开发日志
└── README.md               # 本作品说明
~~~

主要文件：

| 文件 | 作用 |
|---|---|
| app/robot_voice/robot_voice_main.c | 语音入口、Agent 接入和语音诊断命令 |
| app/robot_voice/robot_voice_capture.c | I2S0 麦克风采集 |
| app/robot_voice/robot_voice_afe.c | 音频前处理、VAD 和相关语音处理 |
| app/robot_voice/robot_wakeword.c | 唤醒词状态机、窗口和分数处理 |
| app/robot_voice/robot_wakeword_frontend.c | 唤醒模型输入特征前处理 |
| app/robot_voice/robot_wakeword_tflm.cc | TensorFlow Lite Micro 推理 |
| app/robot_voice/robot_wakeword_model_data.cc | 唤醒模型数据 |
| app/robot_voice/mimo_asr.c | MiMo ASR 请求 |
| app/robot_voice/mimo_llm_stream.c | MiMo v2.6 Flash 流式请求 |
| app/robot_voice/mimo_tts.c | MiMo TTS 请求 |
| app/robot_voice/robot_audio_playback.c | I2S1 扬声器播放后端 |
| app/robot_voice/robot_media_player.c | 音乐和 Media 播放实现 |
| app/robot_voice/robot_music_player.c | 本地 WAV 播放 Tool |
| app/robot_motion/robot_motion_tool.c | 运动 Tool |
| app/robot_expression/robot_expression_tool.c | 表情 Tool |
| app/robot_proactive/robot_music_observer.c | 音乐事件观察和陪伴触发 |
| board/contest_board/src/board_voice_audio.c | Media 音频桥接和播放调度 |
| board/contest_board/src/contest_i2s.c | I2S DMA 与数据适配 |
| skills/manifest.json | Skill 优先级和覆盖关系 |
| skills/feishu-send.md | 飞书发送工作流 |
| board/contest_board/scripts/build_with_hal_backport.sh | 固件构建入口 |
| scripts/generate_skill_bundle.py | 将 Markdown Skill 打包进固件 |

## 七、运行方式

以下命令以 openvela 工作区位于 /home/foam/vela/openvela、开发板串口为 /dev/ttyUSB0 为例。

### 7.1 编译

~~~bash
cd /home/foam/vela/openvela

chmod +x contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh
contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh -j8
~~~

固件产物为构建目录中的 nuttx 和 nuttx.bin。

如需完整清理后重新构建：

~~~bash
contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh distclean -j8
~~~

原有 openvela 配置流程仍可使用：

~~~bash
./build.sh vendor/openvela/boards/contest2026_295_board/configs/nsh --cmake menuconfig
./build.sh vendor/openvela/boards/contest2026_295_board/configs/nsh --cmake savedefconfig
~~~

⚠️ 如果当前工作区没有上述 vendor/openvela/... 配置路径，应以 contest 项目构建脚本使用的实际配置路径为准。

### 7.2 烧录

烧录前让开发板进入下载模式，并确认串口设备名正确：

~~~bash
PORT=/dev/ttyUSB0

python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  write-flash --flash-size 16MB --flash-mode dio --flash-freq 40m \
  0x000000 nuttx/nuttx.bin
~~~

如需清空 Flash：

~~~bash
python -m esptool --chip esp32s3 --port "$PORT" erase_flash
~~~

烧录后松开 BOOT/GPIO0，按 EN/RESET，再打开串口：

~~~bash
picocom -b 115200 /dev/ttyUSB0
~~~

### 7.3 手动配置网络和模型

⚠️ 安全合规：README 不保存 Wi-Fi 密码、API Key、飞书 App Secret 或内部服务地址。请使用实际部署环境中的安全配置。

在串口中执行：

~~~text
nsh> ai_agent
vela> set_wifi <SSID> <WIFI_PASSWORD>
vela> set_llm https://token-plan-cn.xiaomimimo.com/v1 mimo-v2.6-flash <API_KEY>
vela> set_voice_asr mimo-v2.5-asr
vela> set_voice_tts mimo-v2.5-tts
vela> set_feishu_app <FEISHU_APP_ID> <FEISHU_APP_SECRET>
vela> voice_start
~~~

先使用文本验证 Agent：

~~~text
vela> ask hello
~~~

测试完成后停止语音监听：

~~~text
vela> voice_stop
~~~

### 7.4 自动启动

当板级配置启用自动启动时，系统会在开机后自动执行 Agent、Wi-Fi、LLM、ASR、TTS、飞书和语音监听初始化。

自动启动使用的凭据配置位于
`board/contest_board/configs/nsh/defconfig`。提交到仓库时只保留占位符，实际部署前必须由使用者替换为自己的凭据：

~~~config
CONFIG_AI_AGENT_AUTOSTART_FEISHU_APP_ID="<FEISHU_APP_ID>"
CONFIG_AI_AGENT_AUTOSTART_FEISHU_APP_SECRET="<FEISHU_APP_SECRET>"
CONFIG_AI_AGENT_AUTOSTART_LLM_API_KEY="<LLM_API_KEY>"
~~~

上述占位符以及类似 `xxxxxxxx` 的示例值不能用于实际认证。烧录前请在本地配置中填写真实值，并确认这些值不会进入公开提交、补丁或日志。也可以不启用自动启动凭据，开机后通过 `set_llm` 和 `set_feishu_app` 手动配置。

### 7.5 飞书配置

1. 在飞书开放平台创建机器人应用；
2. 启用机器人能力和群聊相关权限；
3. 配置事件流或 WebSocket 事件接收；
4. 获取 App ID 和 App Secret；
5. 在设备上执行 set_feishu_app；
6. 将机器人添加到目标群聊；
7. 在群聊中使用 @aidog 发送测试指令。

测试示例：

~~~text
@aidog 向前一步走
@aidog 播放音乐
@aidog 查询北京天气
~~~

语音发送消息测试：

~~~text
请查询北京的天气，并发到飞书。
请播放一首歌，并把歌曲信息发送到飞书。
请把当前时间发到飞书。
~~~

### 7.6 运动、表情和音乐测试

~~~text
vela> robotctl status
vela> robotctl forward
vela> robotctl left 1 2000
vela> ask 向前走一步
vela> ask 摇一下尾巴
vela> ask 显示开心表情
vela> ask 播放音乐
~~~

测试固件内置的 /etc/media/test.wav：

~~~text
vela> mediad &
vela> mediatool
mediatool> open Music
mediatool> prepare 0 url /etc/media/test.wav
mediatool> start 0
~~~

## 八、关键日志

| 日志前缀 | 用途 |
|---|---|
| [BOOT-DIAG]、[BOARD-*] | 系统和板级初始化 |
| [RV-CAP]、[RV-AFE] | 麦克风采集和音频前处理 |
| [RV-WAKE]、[KWS-*] | 唤醒词窗口、门限和模型结果 |
| [RV-ASR] | ASR 请求和识别结果 |
| [LLM-JSON]、[Flash]、[agent] | LLM、流式响应、Agent 和 Tool Call |
| [TTS流水线]、[TTS合成] | TTS 请求和音频合成 |
| [feishu]、[feishu_recv]、[feishu_send] | 飞书连接、接收和发送 |
| [ROBOT-MOTION]、[POWER-SAFE] | 运动、舵机和动作保护 |
| [ROBOT-EVENT]、[ROBOT-PROACTIVE] | 事件和主动陪伴 |
| [RV-EXPR]、[RV-OLED] | 表情和 OLED |
| [RV-MUSIC-NET]、[RV-MEDIA] | 音乐网络接收和媒体播放 |
| [C-I2S]、[MEDIA-BRIDGE]、[media] | I2S、Media 音频桥和播放状态 |
| [WLAN-RX]、[WLAN-TXDONE] | WLAN 接收、发送完成和网络并发问题 |

## 九、关键问题与解决方案

### 9.1 WLAN 发送导致音频卡顿

播放过程中可能出现 WLAN 发送完成日志。问题不在于单条发送成功日志，而在于网络发送、音频数据搬运和播放时序之间的竞争，可能导致 Media 播放缓冲不足并触发 xrun。

作品侧通过以下方式缓解：

- 增加音频预缓冲；
- 使用独立播放调度；
- 区分暂停、恢复和终止；
- 缩短音频缓冲持有时间；
- 保留 WLAN 诊断日志；
- 不修改官方 WLAN 源文件。

### 9.2 停止播放后仍被恢复

播放停止和 xrun 自动恢复可能产生竞态。

作品侧将终止停止和非终止暂停区分开：

- 终止停止后关闭当前播放会话；
- 已结束会话拒绝后续恢复请求；
- 播放结束后回到 Agent 或语音状态；
- 避免旧 Media 命令重新打开已经结束的播放。

### 9.3 人声打断音乐

语音采集开始时主动停止或暂停 Media 播放，避免音乐和用户语音、TTS 同时输出。

音乐播放音量在作品侧降低，减少语音采集和扬声器输出之间的干扰。

### 9.4 语音唤醒误触发

KWS 使用固定窗口、RMS 门限、分数平均、迟滞和冷却时间减少环境音触发。

训练和验证数据应使用与板端一致的采样率、窗口、FFT、Mel 特征和归一化流程。

⚠️ 待确认：实际部署时仍需使用目标环境中的键盘声、机械声、音乐声和人声进行误触发测试，并重新校准阈值。

## 十、AI Coding 使用说明

本作品在需求拆解、方案设计、编码、调试和文档整理环节使用 AI 辅助开发，完整对话日志保存在 logs/ 目录。

### 10.1 需求拆解与方案设计

围绕常开语音、语音打断音乐、连续播放、机器人动作、表情反馈和飞书控制等需求，将系统拆分为：

- 语音采集；
- KWS；
- ASR；
- Agent；
- Skill；
- Tool；
- TTS；
- Media；
- 飞书消息；
- 硬件执行。

### 10.2 编码与集成

AI 协助生成和审查：

- 语音入口；
- KWS 和音频前处理；
- ASR/TTS 调用；
- MiMo v2.6 Flash 流式请求；
- 飞书事件接收和发送；
- Media bridge；
- 动作 Tool；
- 表情 Tool；
- 音乐观察器；
- Skill 打包和安装逻辑。

实现时遵循“作品侧增加适配和保护逻辑，不修改官方 WLAN 驱动文件”的约束，减少对 openvela 基础组件的侵入。

### 10.3 日志调试

针对以下日志定位系统问题：

- [WLAN-TXDONE]
- [WLAN-RX]
- [MEDIA-BRIDGE]
- xrun
- underflow
- voice_start
- [feishu_recv]
- Agent Tool 日志
- 舵机和掉电保护日志

### 10.4 文档与复现

README 提供：

- 目录结构；
- 硬件连接；
- 编译烧录命令；
- 自动启动说明；
- 飞书配置步骤；
- 串口测试步骤；
- 日志含义；
- 已知限制。

## 十一、硬件配置

| 模块 | 配置 |
|---|---|
| 主控 | ESP32-S3 N16R8 |
| 麦克风 | INMP441，I2S0 RX |
| 功放 | MAX98357，I2S1 TX |
| OLED | SSD1306 128×64，I2C0 |
| 舵机 | 4 路腿部舵机 + 1 路尾巴舵机，LEDC PWM |
| 音频设备 | /dev/audio/pcm0p |

主要引脚：

| 用途 | GPIO |
|---|---:|
| 麦克风 BCLK / WS / DIN | 16 / 17 / 18 |
| 功放 BCLK / WS / DOUT | 39 / 38 / 40 |
| OLED SDA / SCL | 12 / 13 |
| 右前腿 / 右后腿 | 9 / 10 |
| 左后腿 / 左前腿 | 21 / 47 |
| 尾巴 | 48 |

## 十二、安全与已知限制

### 12.1 凭据保护

以下内容不得提交到 README 或公开仓库：

- Wi-Fi 密码；
- LLM API Key；
- 飞书 App Secret；
- 内部服务地址；
- 设备局域网地址；
- 调试端口和私有连接参数。

请使用以下占位符：

~~~text
<SSID>
<WIFI_PASSWORD>
<LLM_ENDPOINT>
<API_KEY>
<FEISHU_APP_ID>
<FEISHU_APP_SECRET>
~~~

### 12.2 网络依赖

ASR、LLM、TTS、在线音乐和飞书功能依赖 Wi-Fi、DNS、TLS 和远程服务。

网络异常时，Agent 可能需要重试。网络服务恢复不代表远程模型服务一定立即可用。

### 12.3 音频和音乐限制

音乐播放会受到以下因素影响：

- Wi-Fi 接收吞吐；
- HTTP/TLS 接收间隔；
- MP3 解码连续性；
- 音频播放缓冲；
- I2S DMA 可用内存；
- 语音监听和 TTS 抢占音频设备。

没有经过同一热点、同一歌曲和同一固件的实机测试，不应仅根据日志声称音乐播放完全稳定。

### 12.4 飞书限制

飞书功能需要：

- 正确配置机器人应用；
- 完成所需权限；
- 成功建立事件流连接；
- 将机器人加入目标群聊；
- 使用正确的 @aidog 消息格式。

## 十三、作品总结

~~~text
听：INMP441 + KWS + MiMo ASR
想：openvela ai_agent + MiMo v2.6 Flash + Skills
做：Robot Tools → 运动 / 表情 / 音乐 / 飞书
说：MiMo TTS + MAX98357
陪伴：音乐事件 + 尾巴动作 + 表情反馈
~~~

VoicePet 在 ESP32-S3 N16R8 上实现了从语音唤醒、语音识别、大模型推理到实体动作、表情显示、音乐播放、飞书交互和语音输出的完整闭环，展示了 openvela 在 AI 硬件产品中的落地方式和扩展能力。
