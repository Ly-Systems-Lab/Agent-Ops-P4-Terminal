/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 音量 / 亮度的持久化 —— 「每次开机保持上一次关机前的值」。
 *
 * 现状（改之前）：
 *   - 音量：`main.cpp` 开机写死 `bsp_extra_codec_volume_set(CODEC_DEFAULT_VOLUME=80)`，
 *           用户在「设置 → Sound」拖过的值关机就丢。
 *   - 亮度：`main.cpp` 开机 `bsp_display_backlight_on()` = 100%，同样会丢。
 *   （顺带一个连带问题：`bsp_display_brightness_get()` 读的是那个静态变量，
 *     开机被写成 100，所以 Display 页永远显示 100% —— 现在读到的就是真值了。）
 *
 * 取值范围**与两个滑条完全一致**，这样「存进去的值」一定能被 UI 原样显示出来：
 *   - 音量   0..95   （SoundPage 的 `lv_slider_set_range(0, 95)`）
 *   - 亮度  20..100  （DisplayPage 的 `lv_slider_set_range(20, 100)`）
 *
 * 写入时机：只在**松手**（`LV_EVENT_RELEASED`）时写一次。
 * 拖动的每一帧都会发 `LV_EVENT_VALUE_CHANGED`，逐帧 `nvs_commit()` 是白白磨损 flash。
 *
 * 调用前必须已经 `nvs_flash_init()` 成功（`main.cpp` 在注册 display 之前就做了）。
 */
#pragma once

#include <cstdint>

#include "nvs.h"
#include "nvs_flash.h"

namespace esp_brookesia::apps::persisted_settings {

constexpr const char *NVS_NAMESPACE = "system";
constexpr const char *NVS_KEY_VOLUME = "volume";
constexpr const char *NVS_KEY_BRIGHTNESS = "brightness";

/* —— 与滑条范围保持一致，改滑条时这两组必须一起改 —— */
constexpr int VOLUME_MIN = 0;
constexpr int VOLUME_MAX = 95;
constexpr int VOLUME_DEFAULT = 80; /* == bsp_board_extra.h 的 CODEC_DEFAULT_VOLUME */

constexpr int BRIGHTNESS_MIN = 20;
constexpr int BRIGHTNESS_MAX = 100;
constexpr int BRIGHTNESS_DEFAULT = 100;

namespace internal {

inline int clamp(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

/** 读一个整型键；不存在/打不开/读失败都返回 `fallback`。 */
inline int loadInt(const char *key, int fallback)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return fallback;
    }

    int32_t value = fallback;
    const esp_err_t err = nvs_get_i32(handle, key, &value);
    nvs_close(handle);

    return (err == ESP_OK) ? static_cast<int>(value) : fallback;
}

inline bool saveInt(const char *key, int value)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_i32(handle, key, static_cast<int32_t>(value));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err == ESP_OK;
}

} // namespace internal

/** 上次保存的音量；没存过 / 越界都退回默认值。 */
inline int loadVolume(void)
{
    return internal::clamp(internal::loadInt(NVS_KEY_VOLUME, VOLUME_DEFAULT),
                           VOLUME_MIN, VOLUME_MAX);
}

inline bool saveVolume(int volume)
{
    return internal::saveInt(NVS_KEY_VOLUME, internal::clamp(volume, VOLUME_MIN, VOLUME_MAX));
}

/** 上次保存的亮度；没存过 / 越界都退回 100%。 */
inline int loadBrightness(void)
{
    return internal::clamp(internal::loadInt(NVS_KEY_BRIGHTNESS, BRIGHTNESS_DEFAULT),
                           BRIGHTNESS_MIN, BRIGHTNESS_MAX);
}

inline bool saveBrightness(int brightness)
{
    return internal::saveInt(NVS_KEY_BRIGHTNESS,
                             internal::clamp(brightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX));
}

} // namespace esp_brookesia::apps::persisted_settings
