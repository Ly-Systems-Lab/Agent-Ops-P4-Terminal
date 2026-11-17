/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 屏幕方向（横屏 / 竖屏）的持久化设置 —— Settings → Display 里那个切换按钮用。
 *
 * 为什么只能「保存 + 重启」，不能热切换：
 *   `esp_lv_adapter` 在注册 display 时就把 rotation 算进了 LVGL 逻辑分辨率
 *   （`src/display/display_manager.c:919-922`），并在创建 bridge 时把 rotation
 *   快照进自己的 runtime（`src/display/bridge/common/display_bridge_common.c:686`）。
 *   适配器**没有**运行时改旋转的 API，也没有任何地方再读 profile.rotation。
 *   ⇒ 热切换必须拆掉整个 LVGL display 再重建（等于重启，但风险高得多）。
 *   所以按钮 = 写 NVS → esp_restart()，开机时 `main.cpp` 读回来再注册 display。
 *
 * NVS 里存的是**方向语义**，不是适配器角度的字面量：
 *   90 ⇒ 逻辑 1280x800（横屏；本项目的现有锚点，也是"键不存在"时的默认）
 *    0 ⇒ 逻辑 800x1280（竖屏，面板原生方向）
 * 注意：横屏在 `main.cpp` 里映射成 `ESP_LV_ADAPTER_ROTATE_270`
 *    而不是 90 —— 本板摄像头在面板下方，转 180° 之后摄像头才朝下。
 *    **NVS 的值故意保持 90**：它是"横屏"的记号，改字面量会让已保存的方向读成竖屏。
 */
#pragma once

#include <stdint.h>

#include "nvs.h"
#include "nvs_flash.h"

namespace esp_brookesia::apps::display_orientation {

constexpr const char *NVS_NAMESPACE = "display";
constexpr const char *NVS_KEY_ROTATION = "rotation";

constexpr uint16_t ROTATION_PORTRAIT = 0;    /* 逻辑 800x1280  */
constexpr uint16_t ROTATION_LANDSCAPE = 90;  /* 逻辑 1280x800  */

inline bool isValid(uint16_t deg)
{
    return (deg == ROTATION_PORTRAIT) || (deg == ROTATION_LANDSCAPE);
}

/**
 * 读保存的方向。任何异常（命名空间/键不存在、值不合法、NVS 没初始化）都退回横屏，
 * 也就是当前的出厂锚点 —— 这样第一次升级固件时用户看到的行为不会变。
 *
 * 调用前必须已经 `nvs_flash_init()` 成功（main.cpp 里已把它提前到 display 注册之前）。
 */
inline uint16_t loadRotation(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ROTATION_LANDSCAPE;
    }

    int32_t value = ROTATION_LANDSCAPE;
    esp_err_t err = nvs_get_i32(handle, NVS_KEY_ROTATION, &value);
    nvs_close(handle);

    if (err != ESP_OK || !isValid(static_cast<uint16_t>(value))) {
        return ROTATION_LANDSCAPE;
    }
    return static_cast<uint16_t>(value);
}

inline bool saveRotation(uint16_t deg)
{
    if (!isValid(deg)) {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_i32(handle, NVS_KEY_ROTATION, static_cast<int32_t>(deg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err == ESP_OK;
}

/** 显示在 UI 上的名字（必须是 ASCII —— montserrat 没有中文字形）。 */
inline const char *name(uint16_t deg)
{
    return (deg == ROTATION_PORTRAIT) ? "Portrait" : "Landscape";
}

} // namespace esp_brookesia::apps::display_orientation
