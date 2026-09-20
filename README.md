# 基于 openvela 的 AI 智能机器狗

## 一、作品简介

VoicePet 是一款基于 openvela、ESP32-S3 和 MiMo V2.5 的具身智能机器狗。它把语音理解、大模型 Agent、机器人动作、OLED 表情、音乐播放和语音播报连接成完整闭环，让机器狗不仅能回答问题，还能根据对话调用真实硬件能力。

作品的主要亮点：

- 使用 openvela 官方 `ai_agent`，统一处理文本和语音交互；
- 集成 MiMo V2.5 ASR、LLM 和 TTS；
- `voice_start` 开启后持续进行语音输入，将采集到的人声送入 ASR 和 Agent；
- 人声输入时停止音乐播放，避免语音交互和音乐同时输出；
- 解决 Media 音频播放在 WLAN 发送完成日志出现时的卡顿问题，未修改官方 WLAN 源文件；
- 支持前进、后退、左转、右转、安全摇尾巴和 OLED 表情；
- 支持本地 WAV、openvela Media 播放，以及饮食推荐、机器人陪伴等 Markdown Skill。

## 二、选题方向

**AI 硬件产品创新**。

本作品将大模型从纯软件对话扩展到真实机器人：Agent 负责理解意图和选择能力，Tool 负责执行动作，硬件负责运动、显示、采集和播报。作品重点解决了嵌入式 AI 产品中的语音交互闭环、硬件安全、音频连续播放和多模块协同问题。

## 三、功能与系统架构

### 3.1 语音交互链路

```text
INMP441 麦克风
    ↓ I2S0 RX
robot_voice_capture
    ↓
MiMo V2.5 ASR
    ↓
openvela ai_agent
    ↓
MiMo V2.5 LLM → Skills / Tool Calls
    ↓
MiMo V2.5 TTS
    ↓
robot_audio_playback → I2S1 TX → MAX98357 → 扬声器
```

麦克风输入为 16 kHz、单声道、PCM16；语音输出为 24 kHz、单声道、PCM16。文本输入和语音输入共用同一个 Agent、Skill、Tool 和回复链路。

### 3.2 常开语音与音乐策略

`voice_start` 开启后，系统进入常开语音输入状态，持续采集人声并交给 ASR 和 Agent 处理。人声进入录音流程时，Media 播放会收到停止请求，优先保证语音交互清晰。

### 3.3 Agent Tool

| Tool | 能力 | 主要实现 |
|---|---|---|
| `robot_move` | 前进、后退、左转、右转 | `robot_motion_tool.c` |
| `robot_tail_wag` | 安全摇尾巴 | `robot_motion_tool.c` |
| `robot_set_expression` | 设置持续表情 | `robot_expression_tool.c` |
| `robot_react` | 短时情绪反应 | `robot_expression_tool.c` |
| `robot_play_music` | 播放本地 WAV | `robot_music_player.c` |
| `music_search` | 在线搜索音乐 | openvela Tool |
| `music_play` | 调用 Media 播放 | openvela Tool |

运动使用统一 worker 和 `robot_action_guard` 管理；LLM 只调用高层动作，不直接操作任意 PWM 或舵机角度。

## 四、目录结构

```text
contest2026_295_suanliheidong/
├── app/
│   ├── robot_voice/        # 麦克风、ASR、LLM、TTS、音频播放和语音入口
│   ├── robot_motion/       # 步态、运动 worker 和运动 Tool
│   ├── robot_expression/   # 表情状态、OLED 绘制和表情 Tool
│   ├── robotctl/            # 手工运动调试命令
│   ├── robot_core/          # 动作保护和网络适配
│   └── robot_proactive/     # Skill 安装和主动交互
├── board/contest_board/     # ESP32-S3 板级初始化、I2S、音频桥和引脚定义
├── skills/                  # Markdown 格式的自定义 Skill
├── scripts/                 # Skill 打包等构建脚本
├── docs/                    # 设计和调试文档
├── logs/                    # AI Coding 对话和开发日志
└── README.md                # 本作品说明
```

主要文件：

| 文件 | 作用 |
|---|---|
| `app/robot_voice/robot_voice_main.c` | 语音入口、Agent 接入和语音诊断命令 |
| `app/robot_voice/robot_voice_capture.c` | I2S0 麦克风采集 |
| `app/robot_voice/mimo_asr.c` | MiMo ASR 请求 |
| `app/robot_voice/mimo_tts.c` | MiMo TTS 请求 |
| `app/robot_voice/robot_audio_playback.c` | I2S1 扬声器播放后端 |
| `app/robot_voice/robot_music_player.c` | 本地 WAV 播放 Tool |
| `app/robot_motion/robot_motion_tool.c` | 运动 Tool |
| `app/robot_expression/robot_expression_tool.c` | 表情 Tool |
| `board/contest_board/src/board_voice_audio.c` | Media 音频桥接和播放调度 |
| `board/contest_board/src/contest_i2s.c` | I2S DMA 与数据适配 |
| `board/contest_board/scripts/build_with_hal_backport.sh` | 固件构建入口 |
| `scripts/generate_skill_bundle.py` | 将 Markdown Skill 打包进固件 |

## 五、运行方式

以下命令以 openvela 工作区位于 `/vela/openvela`、开发板串口为 `/dev/ttyUSB0` 为例。

### 5.1 编译

```bash
cd /vela/openvela
python3 -m venv myenv
source myenv/bin/activate

./build.sh vendor/openvela/boards/contest2026_295_board/configs/nsh --cmake menuconfig
./build.sh vendor/openvela/boards/contest2026_295_board/configs/nsh --cmake savedefconfig

chmod +x contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh
contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh -j8
```

固件产物为 `nuttx/nuttx` 和 `nuttx/nuttx.bin`。需要完整清理时执行：

```bash
contest2026_295_suanliheidong/board/contest_board/scripts/build_with_hal_backport.sh distclean -j8
```

### 5.2 烧录

烧录前让开发板进入下载模式，并确认串口设备名正确：

```bash
PORT=/dev/ttyUSB0
python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  write-flash --flash-size 16MB --flash-mode dio --flash-freq 40m \
  0x000000 nuttx/nuttx.bin
```

如需清空 Flash，可先执行：

```bash
python -m esptool --chip esp32s3 --port "$PORT" erase_flash
```

烧录后松开 BOOT/GPIO0，按 EN/RESET，再打开串口：

```bash
picocom -b 115200 /dev/ttyUSB0
```

### 5.3 网络、模型和语音配置

在串口中执行：

```text
nsh> ai_agent
vela> set_wifi <SSID> <密码>
vela> set_llm https://token-plan-cn.xiaomimimo.com/v1 mimo-v2.5 <API_KEY>
vela> set_voice_asr mimo-v2.5-asr
vela> set_voice_tts mimo-v2.5-tts
```

先用文本验证 Agent：

```text
vela> ask hello
```

再启动常开语音：

```text
vela> voice_start
```

此时直接说出指令或问题；测试完成后执行：

```text
vela> voice_stop
```

### 5.4 运动、表情和音乐测试

```text
vela> robotctl status
vela> robotctl forward
vela> robotctl left 1 2000
vela> ask 向前走一步
vela> ask 摇一下尾巴
vela> ask 显示开心表情
```

测试固件内置的 `/etc/media/test.wav`：

```text
vela> mediad &
vela> mediatool
mediatool> open Music
mediatool> prepare 0 url /etc/media/test.wav
mediatool> start 0
```

### 5.5 关键日志

| 日志前缀 | 用途 |
|---|---|
| `[BOOT-DIAG]`、`[BOARD-*]` | 系统和板级初始化 |
| `[RV-CAP]`、`[RV-ASR]` | 麦克风采集和 ASR |
| `[LLM-JSON]`、`[agent]` | LLM、Agent 和 Tool Call |
| `[ROBOT-MOTION]`、`[POWER-SAFE]` | 运动和动作保护 |
| `[RV-EXPR]`、`[RV-OLED]` | 表情和 OLED |
| `[C-I2S]`、`[MEDIA-BRIDGE]`、`[media]` | I2S、Media 音频桥和播放状态 |
| `[WLAN-TXDONE]` | WLAN 发送完成状态，用于定位网络与音频并发问题 |

## 六、关键问题与解决方案

### 6.1 WLAN 发送导致音频卡顿

播放过程中会出现 `[WLAN-TXDONE]` 日志。问题并不是发送成功本身，而是网络发送、音频数据搬运和播放时序竞争，导致 Media 播放缓冲不足并触发 xrun。解决方式是在作品侧增加音频缓冲、预缓冲和播放调度，并让 Media bridge 对暂停、恢复、结束状态进行明确区分；没有修改官方 WLAN 源文件。

### 6.2 停止播放后仍被恢复

播放停止和 xrun 自动恢复可能存在竞态。作品侧把终止停止和非终止暂停区分开：收到终止停止后立即关闭当前播放会话，后续恢复请求不能重新打开已经结束的会话，从而回到 Agent/语音状态。

### 6.3 人声打断音乐

语音采集开始时主动停止 Media 播放，并保留音频播放状态，避免音乐与用户语音、TTS 同时输出。音乐播放音量在作品侧做了降低处理，提升语音交互时的舒适度。

## 七、AI Coding 使用说明

本作品在需求拆解、方案设计、编码、调试和文档整理环节均使用 AI 辅助开发，完整对话日志保存在 `logs/` 目录。

### 7.1 需求拆解与方案设计

围绕“常开语音、语音打断音乐、连续播放、机器人动作和表情反馈”等需求，先与 AI 拆分出语音采集、ASR、Agent、Tool、TTS、Media 和硬件执行几个状态域，再确定它们之间的消息和状态边界。

### 7.2 编码与集成

AI 协助生成和审查语音入口、ASR/TTS 调用、常开语音状态、Media bridge、动作 Tool、表情 Tool 和 Skill 打包相关代码。实现时遵循“只在作品侧增加适配和保护逻辑，不改官方 WLAN 文件”的约束，减少对 openvela 基础组件的侵入。

### 7.3 日志调试

针对 `[WLAN-TXDONE]`、`[MEDIA-BRIDGE]`、`xrun`、`underflow`、`voice_start` 和 Agent Tool 日志，AI 协助定位音频缓冲不足、暂停/恢复竞态和语音状态切换问题，并据此调整预缓冲、播放节奏、结束状态和日志点。

### 7.4 文档与复现

AI 协助整理目录结构、硬件连接、编译烧录命令、串口测试步骤、日志含义和已知限制，使评委可以从源码、固件和串口命令复现主要功能。

## 八、硬件配置

| 模块 | 配置 |
|---|---|
| 主控 | ESP32-S3 |
| 麦克风 | INMP441，I2S0 RX |
| 功放 | MAX98357，I2S1 TX |
| OLED | SSD1306 128×64，I2C0 |
| 舵机 | 4 路腿部舵机 + 1 路尾巴舵机，LEDC PWM |
| 音频设备 | `/dev/audio/pcm0p` |

主要引脚：

| 用途 | GPIO |
|---|---:|
| 麦克风 BCLK / WS / DIN | 16 / 17 / 18 |
| 功放 BCLK / WS / DOUT | 39 / 38 / 40 |
| OLED SDA / SCL | 12 / 13 |
| 右前腿 / 右后腿 | 9 / 10 |
| 左后腿 / 左前腿 | 21 / 47 |
| 尾巴 | 48 |

## 九、作品总结

```text
听：INMP441 + MiMo ASR
想：openvela ai_agent + MiMo LLM + Skills
做：Robot Tools → 运动 / 表情 / 音乐
说：MiMo TTS + MAX98357
```

VoicePet 在 ESP32-S3 上实现了从语音输入、模型推理到实体动作和语音输出的完整闭环，重点展示了 openvela 在 AI 硬件产品中的落地方式和可扩展性。
