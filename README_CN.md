# Muse 随身手表中文增强版（StopWatch / ESP32 中文与中转分支）

> **English Overview**: A Chinese-enhanced community fork of [`facebookincubator/muse-gadget-sdk`](https://github.com/facebookincubator/muse-gadget-sdk) (Apache-2.0) focused on **M5Stack StopWatch** (466×466 round AMOLED) and ESP32-S3 Muse gadgets. Key additions include a lightweight on-device VMess TCP relay (`components/vproxy`), synchronized smooth-scrolling Simplified Chinese subtitles with swipe-right chat history, Noto Sans SC CJK font support, microphone gain/click-noise tuning, and extensive stability fixes for live voice calls.

本分支（`vproxy`）基于 Meta 开源的 [muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)（Apache License 2.0）进行深度二次开发，主要面向 **M5Stack StopWatch（466×466 圆屏触摸手表）** 等 ESP32-S3 设备，解决中文显示、国内网络连通与实时语音通话中的各类稳定性问题。

> 原版英文文档请见 [README.md](README.md)。

---

## 相比官方原版的核心增强

### 1. 中文显示与随声慢滚字幕
- 集成 **Noto Sans SC** 简体中文字库（存于 Flash，不额外占用宝贵内部 RAM），彻底告别中文方块乱码。
- 为 M5Stack StopWatch 新增**随声慢滚字幕**与**右滑查看历史聊天记录**（`esp32/components/muse/muse_watch_dialogue.c`）。
- 补齐实时语音通话界面与状态提示的简体中文译文，修复标点显示方块与尾句丢失问题。

### 2. 轻量板载网络中转组件（`components/vproxy`）
- 新增嵌入式轻量代理转发模块（`esp32/components/vproxy/`），在内存受限的 ESP32-S3 上通过本地回环套接字完成透明中转，解决特定网络环境下无法直连 Muse 云端服务的问题。
- 配套提供 `esp32/devices/sdkconfig.vproxy` 配置叠加文件，精细调优 lwIP 套接字数、Wi-Fi 收发缓存与 PSRAM 任务栈分配，确保 16 KB+ DMA 内存余量稳定注册。
- 节点参数全部存放在设备本地 NVS 分区，**源码中不包含任何节点地址或密钥**。

### 3. 实时通话与底层稳定性深度修复
- **防卡死与状态机加固**：修复语音输入被云端过滤后卡在“想一想”、无回复通话不自动退出、尾音状态跳变、收尾吞黄键、跨核竞争等问题。
- **音频与按键优化**：针对手表硬件调优麦克风收音与黄键机械点击声处理，延长通话保活窗口，兼顾灵敏度与清晰度。
- **渲染与抓屏修复**：修复慢滚字幕尾句丢失、关中断锁、小图残影、视频解码黑屏以及串口抓屏卡死/残行问题。

---

## 主要新增与改动模块

```text
esp32/
├── components/
│   ├── vproxy/                     # 新增：轻量板载中转与 VMess 协议实现
│   │   ├── vproxy.c / vproxy.h
│   │   ├── vmess.c / vmess.h
│   │   └── Kconfig
│   └── muse/
│       ├── muse_watch_dialogue.c/h # 新增：手表随声慢滚字幕与右滑聊天记录
│       └── muse_voice_call_core.c/h# 加固：实时语音通话状态机与超时/过滤恢复
├── devices/
│   ├── sdkconfig.muse-m5stack-stopwatch
│   └── sdkconfig.vproxy            # 新增：中转模式 + 中文字库 + 内存调优配置
├── main/
│   ├── muse_glue.c                 # 中转接入与通话事件桥接
│   ├── noise_control.cpp           # 麦克风噪声/按键声处理
│   └── sdk_token_console.c         # 串口令牌配置辅助
└── tests/                          # 新增：vproxy / 字幕 / 电量 / 按键回归测试
```

---

## 编译与烧录（以 M5Stack StopWatch 为例）

### 1. 环境要求
- [ESP-IDF v6.0+](https://docs.espressif.com/projects/esp-idf/)

### 2. 叠加配置编译
在 `esp32/` 目录下，按目标板型叠加 `sdkconfig.vproxy`（放在最后以生效中文字库与内存调优）：

```bash
cd esp32
source ~/esp/esp-idf-v6/export.sh

idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse-m5stack-stopwatch;devices/sdkconfig.vproxy" \
  set-target esp32s3

idf.py build
idf.py -p /dev/cu.usbmodemXXXX app-flash
```

### 3. 自行配置网络与中转节点
- Wi-Fi、Muse 配对凭据与中转节点配置均存储在设备本机 NVS 中，烧录固件后通过串口命令或配套配网方式写入即可。
- **安全提醒**：请勿将自己的 `.sdk-token`、Wi-Fi 密码或代理节点信息提交到公开代码仓库（`.gitignore` 已默认忽略 `.sdk-token` 与 `/backups/`）。

---

## 开源协议与致谢

- **中文增强分支主导与实测**：[@dawangjiaowolaixunshan-dot](https://github.com/dawangjiaowolaixunshan-dot)（协作开发：Google Antigravity & Claude Code）
- 本项目基于 Meta Platforms, Inc. 开源的 [`facebookincubator/muse-gadget-sdk`](https://github.com/facebookincubator/muse-gadget-sdk) 二次开发。
- 遵循原仓库的 **Apache License 2.0** 协议，详见 [LICENSE](LICENSE)。
