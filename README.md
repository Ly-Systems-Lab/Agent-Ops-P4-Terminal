# Agent-Ops-P4-Terminal

**一台能听懂指令、也能被 Agent 驱动的触控终端 —— ESP32-P4 固件。**

🔗 **三件套** · [🔭 总蓝图 · Architecture](https://github.com/Ly-Systems-Lab/Agent-Ops-Architecture) · [🧠 控制面 · WeCom-Bridge](https://github.com/Ly-Systems-Lab/Agent-Ops-WeCom-Bridge) · [🔌 数据面 · Edge-Hub](https://github.com/Ly-Systems-Lab/Agent-Ops-Edge-Hub) · **🖥️ 端侧终端（本仓）**

Agent-Ops 是一套分层的 Agent 编排结构：控制面管「人怎么指挥 Agent」，数据面管「Agent 怎么指挥设备」，
本仓是这套结构里**被 Agent 指挥的那一端** —— 一台放在桌面上的 ESP32-P4 触控平板。

> 在本项目里，「Agent-Ops」指端侧 Agent 的控制面/终端分层，与 `agentops.ai` 的观测平台无关。

---

## 它长什么样

微雪 **ESP32-P4-WIFI6-Touch-LCD-10.1**（SKU 33672）出厂固件（ESP-Brookesia 手机风格桌面）的二次开发。
保留出厂的桌面与应用框架，在此之上加了自己的应用与服务：

| | |
|---|---|
| 🖼️ **桌面** | 应用网格、横屏 1280×800、可翻转 180°、壁纸（JPEG / WebP） |
| 📷 **相机** | 实时预览与拍照 |
| 🖼️ **相册** | JPEG / WebP；缩略图网格 → 1:1 明细图 → 缩放查看 |
| 🎬 **视频** | MJPEG/AVI 播放，硬件 JPEG 解码 + PPA 缩放旋转，横竖屏切换 |
| 🎵 **音乐** | MP3 播放，从 SD 卡读取，支持后台播放 |
| ✏️ **画板 / 频谱分析 / 计算器** | 出厂示例应用 |
| 🌐 **网页控制台** | 板子起 httpd，浏览器里管理文件、看设备状态 |
| 💾 **U 盘模式** | 把 SD 卡通过 USB 交给电脑（MSC） |
| ⚙️ **设置** | WLAN、显示方向、音量、亮度等，NVS 持久化 |
| 🗣️ **小智 AI** | 语音对话（走小智官方云，需要联网） |
| 💬 **Hub** | **对 Agent 下指令、看流式回执** —— 本仓与 Agent-Ops 的接点 |
| 🤖 **MQTT 反控** | Agent 可以远程打开音乐 / 打开小智 / 一键清后台 |

WiFi 开机自动连接、断线自愈；网络配置与 broker 参数都存在设备 NVS 里。

---

## 硬件

| 项 | 要求 |
|---|---|
| 板卡 | 微雪 ESP32-P4-WIFI6-Touch-LCD-**10.1**（P4 Rev3.x） |
| Flash | 32 MB（本仓分区表用到 32 MB） |
| PSRAM | 板载（Hex 模式，250 MHz） |
| 存储 | microSD 卡（音乐、视频、相册、壁纸放这里） |
| 工具 | ESP-IDF **v5.5.5** |

> 上游固件同时支持 7 英寸与 8 英寸版本；本仓保留了三者的 `sdkconfig.defaults.lcd-*`，
> 但**只在 10.1 英寸板上验证过**。

---

## 快速开始

### A. 直接刷预编译固件

到 [Releases](https://github.com/Ly-Systems-Lab/Agent-Ops-P4-Terminal/releases) 下载固件包并解压，然后：

```bash
pip install esptool
esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash @flash_args
```

固件包里的六个分段（`flash_args` 就是这张表）：

| 地址 | 文件 | 作用 |
|---|---|---|
| `0x2000` | `bootloader.bin` | 引导 |
| `0x8000` | `partition-table.bin` | 分区表 |
| `0x10d000` | `ota_data_initial.bin` | OTA 元数据 |
| `0x110000` | `srmodels.bin` | 语音唤醒模型 |
| `0x200000` | `esp32-p4-lcd-x-brookesia.bin` | **应用** |
| `0xa00000` | `storage.bin` | 出厂 SPIFFS 内容 |

后续只升级应用时，只写 `0x200000` 那一段即可。

### B. 自己编译

```bash
git clone https://github.com/Ly-Systems-Lab/Agent-Ops-P4-Terminal.git
cd Agent-Ops-P4-Terminal
. $IDF_PATH/export.sh          # ESP-IDF v5.5.5

idf.py -B build-lcd-10-1 \
  -D SDKCONFIG="$PWD/build-lcd-10-1/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.rev3_x;sdkconfig.defaults.lcd-10-1" \
  build flash monitor
```

首次构建时，ESP-IDF 组件管理器会按各 `idf_component.yml` 自动拉取依赖组件（约 50 个）。

<details>
<summary>只要一个整片镜像？</summary>

```bash
cd build-lcd-10-1
python -m esptool --chip esp32p4 merge_bin -o merged.bin @flash_args
```

得到的文件从 `0x0` 开始整片写入。
</details>

---

## Hub 与 MQTT

Hub 是桌面上那个对话应用。它在屏幕上显示一条对话流，把用户敲的指令经 MQTT 发给树莓派上的 Agent，
再把流式回执逐帧显示出来。同一个连接上，Agent 还能反向给设备发命令。

主题、报文格式、错误码、反控命令的完整契约见 **[`docs/mqtt-contract.md`](docs/mqtt-contract.md)**。

**没有证书也能用**：`secrets/` 为空时构建照常通过，Hub 顶栏显示「缺证书」，只是不发起 MQTT 连接。
证书的生成方式见 [`secrets/README.md`](secrets/README.md)。

---

## 目录结构

```
components/
  brookesia_core/              桌面与应用框架（源自 ESP-Brookesia，已修改）
  esp32_p4_wifi6_touch_lcd_x/  板级支持包（源自微雪出厂固件源码，已修改）
  bsp_extra/                   音频 / 显示 / SD 等外设封装
  brookesia_app_album/         相册（JPEG / WebP、壁纸）
  brookesia_app_chat/          Hub（对话 UI + MQTT 传输）
  brookesia_hub_service/       MQTT 服务、证书嵌入、连接参数
  brookesia_hub_commands/      Agent 反控命令
  brookesia_net/               开机自连 WiFi 与自愈
  brookesia_web_console/       网页控制台后端
  brookesia_usb_storage/       USB MSC
  libwebp/                     Google libwebp 解码器（vendored）
main/                          应用装配与开机流程
local_components/              本地覆写的托管组件
spiffs/                        出厂 SPIFFS 内容（小智提示音）
secrets/                       MQTT 客户端证书（不入库）
docs/
```

---

## 已知限制

- **视频只支持 MJPEG 编码的 AVI。** P4 有 H.264 **编码器**、没有解码器；单个 AVI 文件不要超过 1 GB
  （播放器只读第一个 `movi` 块，超了会在大约 15 分钟处直接结束）。
- **板子内置字体没有 emoji 字形**，显示到屏幕上的文本建议只用 ASCII；Hub 自带 20 px 中文字库。
- **拼音输入只能逐字选**，没有词组联想。
- **语音输入依赖小智官方云**；端侧只有唤醒词与命令词，没有自由 ASR。
- **MQTT 反控只有三条固定命令**（`music` / `aichat` / `close_all`），没有「打开任意应用 / 点任意按钮」的能力。

---

## 许可

本仓以 **Apache License 2.0** 发布，见 [`LICENSE`](LICENSE)。
第三方组件的许可与逐条说明见 [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md)。

本项目与 Espressif、微雪（Waveshare）均无从属关系，也不是任何一方的官方固件。