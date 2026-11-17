#pragma once
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create new touchscreen
 *
 * **要释放它，必须用下面的 `bsp_touch_del()`，不能用 `esp_lcd_touch_del()`。**
 *
 * （上游原文这里写着 "you can use esp_lcd_touch API, ie. `esp_lcd_touch_del(tp);`"
 *  —— **那句话是错的**，2026-10-16 实测证伪：本函数除了 touch 句柄，还**额外**建了一个
 *  I2C panel-io（内部 `esp_lcd_new_panel_io_i2c()`），而
 *  `esp_lcd_touch_del()` → `esp_lcd_touch_gt911_del()` 只做 GPIO 复位 + `free(tp)`，
 *  **根本不碰那个 io**；偏偏本函数又不把它交出来 ⇒ 调用方**想释放也无从下手**。
 *  照原文写就会每调一次漏一个 io —— Camera / VideoPlayer 两处都是这么写的。
 *  危害见 `bsp_touch_del()` 的说明。）
 *
 * @param[in]  config    touch configuration
 * @param[out] ret_touch esp_lcd_touch touchscreen handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd_touch failure
 */
esp_err_t bsp_touch_new(const bsp_display_cfg_t *cfg, esp_lcd_touch_handle_t *ret_touch);

/**
 * @brief 释放 `bsp_touch_new()` 建的全部资源（touch 句柄 **+ 它内部的 I2C panel-io**）
 *
 * 为什么必须有这个函数：`bsp_touch_new()` 会 `esp_lcd_new_panel_io_i2c()` 造一个 io，
 * 而 `esp_lcd_touch_del()` **不会**释放它 ⇒ **每建一次触摸实例就漏一个 io**。
 * 单次泄漏很小（实测约 200 B/次），但它落在**大块 PSRAM 的空洞之间**，
 * 于是那些大块**再也拼不回一整片连续区** —— 这才是它真正致命的地方。
 * 2026-10-16 实测：
 * 反复开/关相机时，`free` 15 轮一动不动（16938~16940 KB，**没有总量泄漏**），
 * 而 `largest_block` 单调衰减 16896 → 12800 → 10752 → …… → 1984 KB 后**卡死**，
 * 到第 11 轮连一块 2 MB 连续的都拼不出来（相机要 2,048,000 B）⇒ 相机黑屏。
 * **受影响的远不止相机**：相册大图页要 2,880,000~3,612,672 B 连续块，
 *    `largest_block` 掉到 2.8 MB 以下之后它也会一起坏，而且**只有重启板子才恢复**。
 *
 * **只能用于 app 自己临时建的那些触摸实例。**
 *    **绝不能用在主显示的触摸上** —— 那个句柄由 `bsp_display_indev_init()` 交给
 *    `esp_lv_adapter_register_touch()` 持有，LVGL 的 indev 一直在用它。
 *
 * @param[in]  touch  `bsp_touch_new()` 返回的句柄（传 NULL 直接返回错误，不做事）
 * @return
 *      - ESP_OK         On success
 *      - ESP_ERR_INVALID_ARG  touch 为 NULL
 */
esp_err_t bsp_touch_del(esp_lcd_touch_handle_t touch);

#ifdef __cplusplus
}
#endif
