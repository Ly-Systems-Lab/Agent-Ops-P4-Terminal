# 第三方组件与许可

本仓自身以 **Apache License 2.0** 发布（见 [`LICENSE`](LICENSE)）。下面列出它包含或依赖的第三方代码，以及各自的许可条款。

---

## 1. 派生来源与改动声明

本仓是在两套上游代码的基础上二次开发的。依据 Apache-2.0 §4(b)，改动处以注释形式就地标注。

| 上游 | 许可 | 本仓中对应的目录 | 改动 |
|---|---|---|---|
| [ESP-Brookesia](https://github.com/espressif/esp-brookesia)（Espressif）| Apache-2.0 | `components/brookesia_core/` | 已修改：桌面/应用管理器的资源生命周期与关闭路径、显示层的锁与刷新时序 |
| [ESP32-P4-WIFI6-Touch-LCD-X](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-X)（Waveshare）出厂固件源码 | Apache-2.0 | `components/esp32_p4_wifi6_touch_lcd_x/`、`components/bsp_extra/`、`main/`、`sdkconfig.defaults*`、`partitions.csv`、`spiffs/` | 已修改的组件见各文件头部注释 |

其余 `components/` 下的应用组件（`brookesia_app_album`、`brookesia_app_chat`、`brookesia_app_usbdrive`、`brookesia_web_console`、`brookesia_app_webconsole`、`brookesia_hub_service`、`brookesia_hub_commands`、`brookesia_net`、`brookesia_usb_storage` 等）为本仓编写，同样以 Apache-2.0 发布。

本仓与 Waveshare、Espressif 均无从属关系；本项目名称与微雪的商标无关。

---

## 2. 随仓库提供的第三方源码

| 组件 | 版本 | 许可 | 位置 |
|---|---|---|---|
| Google libwebp（**仅解码器**） | 1.5.0 | BSD-3-Clause | `components/libwebp/`，全文见 `components/libwebp/COPYING` |
| chmorgan/esp-audio-player | 1.1.0 | Apache-2.0 | `local_components/esp-audio-player/`，全文见同目录 `LICENSE` |

`components/libwebp/` 只包含 `dec` / `dsp` / `utils` 三个目录的解码路径源码，未包含编码器、SIMD 实现与 demux 工具。

---

## 3. 构建时由组件管理器拉取的依赖

下表组件的**源码不随本仓提供**。首次构建时 ESP-IDF 组件管理器会按 `idf_component.yml` 从
[components.espressif.com](https://components.espressif.com/) 拉取对应版本。

| 组件 | 版本 | 许可 |
|---|---|---|
| `78/xiaozhi-fonts` | 1.6.0 | MIT |
| `chmorgan/esp-audio-player` | 1.1.0 | Apache-2.0 |
| `chmorgan/esp-file-iterator` | 1.0.0 | Apache-2.0 |
| `chmorgan/esp-libhelix-mp3` | 1.0.3 | Apache-2.0 |
| `espressif2022/image_player` | 1.1.1 | Apache-2.0 |
| `espressif/avi_player` | 2.0.0 | Apache-2.0 |
| `espressif/button` | 4.2.3 | Apache-2.0 |
| `espressif/cjson` | 1.7.19~2 | MIT |
| `espressif/cmake_utilities` | 0.5.3 | Apache-2.0 |
| `espressif/dl_fft` | 0.8.0 | MIT |
| `espressif/eppp_link` | 1.1.6 | Apache-2.0 |
| `espressif/esp-boost` | 0.3.1 | Apache-2.0 |
| `espressif/esp-dsp` | 1.8.0 | Apache-2.0 |
| `espressif/esp-lib-utils` | 0.3.0 | Apache-2.0 |
| `espressif/esp-sr` | 2.4.7 | Espressif MIT † |
| `espressif/esp_audio_codec` | 2.4.1 | Espressif Modified MIT † |
| `espressif/esp_audio_effects` | 1.3.0~1 | Espressif Modified MIT † |
| `espressif/esp_cam_sensor` | 2.0.1 | Apache-2.0 |
| `espressif/esp_codec_dev` | 1.5.11 | Apache-2.0 |
| `espressif/esp_h264` | 1.0.4 | Apache-2.0 |
| `espressif/esp_hosted` | 1.4.7 | Apache-2.0 |
| `espressif/esp_ipa` | 1.3.1 | Espressif MIT † |
| `espressif/esp_lcd_jd9365` | 2.0.2 | Apache-2.0 |
| `espressif/esp_lcd_touch` | 1.2.1 | Apache-2.0 |
| `espressif/esp_lcd_touch_gt911` | 1.2.1 | Apache-2.0 |
| `espressif/esp_lv_decoder` | 0.5.1 | Apache-2.0 |
| `espressif/esp_lv_fs` | 1.0.1 | Apache-2.0 |
| `espressif/esp_lvgl_adapter` | 0.6.3 | Apache-2.0 |
| `espressif/esp_mmap_assets` | 1.3.2 | Apache-2.0 |
| `espressif/esp_new_jpeg` | 1.0.2 | Espressif MIT † |
| `espressif/esp_sccb_intf` | 0.0.9 | Apache-2.0 |
| `espressif/esp_serial_slave_link` | 1.1.2 | Apache-2.0 |
| `espressif/esp_tinyusb` | 2.4.0 | Apache-2.0 |
| `espressif/esp_video` | 2.0.1 | Espressif MIT † |
| `espressif/esp_websocket_client` | 1.8.0 | Apache-2.0 |
| `espressif/esp_wifi_remote` | 0.14.5 | Apache-2.0 |
| `espressif/esp_xiaozhi` | 0.1.1 | Apache-2.0 |
| `espressif/freetype` | 2.15.0 | FTL 或 GPLv2（双许可，见 §4） |
| `espressif/gmf_fft` | 1.0.0 | Espressif Modified MIT † |
| `espressif/i2c_bus` | 1.5.2 | Apache-2.0 |
| `espressif/knob` | 1.1.0 | Apache-2.0 |
| `espressif/libpng` | 1.6.59~1 | Zlib |
| `espressif/mcp-c-sdk` | 2.0.1 | Apache-2.0 |
| `espressif/mqtt` | 1.1.0 | Apache-2.0 |
| `espressif/qoi` | 0.20260529.0 | MIT |
| `espressif/tinyusb` | 0.21.0~2 | MIT |
| `espressif/usb_host_uvc` | 2.4.2 | Apache-2.0 |
| `espressif/zlib` | 1.4.0 | Zlib |
| `lvgl/lvgl` | 9.5.0 | MIT |
| `waveshare/esp_lcd_ili9881c` | 2.0.0 | Apache-2.0 |

---

## 4. 三点需要留意的地方

**† Espressif MIT 变体。** 上表标 † 的 7 个组件使用的不是标准 MIT，而是 Espressif 自己的许可文本，其中写明
「permission is hereby granted for use **exclusively with Espressif Systems products**」。
本仓面向 ESP32-P4 硬件，属于该许可允许的范围。**如果你要把这些组件单独用于非 Espressif 平台，需要另行取得授权。**

**FreeType。** `espressif/freetype` 的许可文件给出 **FTL（FreeType License）与 GPLv2 二选一**。
本项目按 **FTL** 使用，因此不引入 GPL 的传染性条款。FTL 要求在使用它的文档中致谢
「Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.」——
本文件即为此用途。

**未包含 GPL / LGPL 组件。** 上表与 §2 的全部条目中，没有仅以 GPL 或 LGPL 发布的组件；
唯一的 GPL 选项是 FreeType 的双许可之一，本项目未选用。

---

## 5. 发布二进制时

预编译固件（GitHub / Gitee Releases 附件）中静态链接了上述部分组件的目标代码。分发该二进制时，
上述许可条款依然适用：需要随固件一并提供本文件与 `LICENSE`（以及 §2 两份全文），
并保留各组件要求的版权声明。Espressif MIT 变体的「仅限 Espressif 产品」限制同样适用于该二进制 ——
固件面向 ESP32-P4，符合这一范围。