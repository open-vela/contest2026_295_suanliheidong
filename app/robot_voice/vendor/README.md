# AFE 第三方源码说明

本目录随应用编译，不依赖 ESP-IDF、系统 Speex 包、在线下载或预编译 Xtensa 库。

| 组件 | 固定来源 | 原始归档 SHA-256 |
|---|---|---|
| SpeexDSP 1.2.1 | https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz | `8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d` |
| libfvad v1.0，WebRTC GMM VAD 的独立移植 | https://github.com/dpirch/libfvad/tree/v1.0 | `8c8a1e4911a454b1a8e3ed062abbce4865eea3fc794417e2c96373309174215c`（GitHub v1.0 tar.gz） |

保留 Speex `COPYING` 与 libfvad `LICENSE`、`PATENTS`、`AUTHORS`。没有修改第三方授权。

Speex 使用 portable floating-point + KISS FFT；应用只使用 NS。`mdf.c` 用于满足
preprocess 的 residual-echo 符号依赖，应用从未建立 echo state，也没有接入 AEC。
`robot_voice_afe_sources.mk` 和 `.cmake` 列出实际编译单元。根目录 `robot_afe_*.c`
是独立编译包装层；`namespace.h` 为外部符号添加 `rvafe_` 前缀，避免与系统库冲突。
配置局限于这些包装层，没有把 `FLOATING_POINT` 或 FFT 宏注入整个 OpenVela 工程。

对 Speex 的本地修改仅为初始化内存不足时的 NULL 检查、完整释放和安全析构，见
`SPEEX_OOM_HARDENING.patch`。`speexdsp_config_types.h` 使用标准定宽整数类型。
libfvad 源码保持原样；`robot_afe_vad_fvad.c` 在包装层增加
`robot_afe_vad_evidence()`，保留原分类返回值：1 是新语音证据，大于 1 是尾音保持。
后者不能累计为独立的起音确认票数。Speex 自带的实验性 VAD 没有启用。
