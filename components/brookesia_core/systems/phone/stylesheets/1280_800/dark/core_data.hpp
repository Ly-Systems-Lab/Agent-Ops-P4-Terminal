/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/base/esp_brookesia_base_context.hpp"
#include "systems/phone/assets/esp_brookesia_phone_assets.h"

namespace esp_brookesia::systems::phone {

constexpr base::Display::Data STYLESHEET_1280_800_DARK_CORE_DISPLAY_DATA = {
    .background = {
        .color = gui::StyleColor::COLOR(0x1A1A1A),
        /* 我们改成 NULL 的（上游原本指向 esp_brookesia_image_large_wallpaper_dark_720_720）。
         * 理由两条，任一都够：
         *   ① **装不下**：那张图是一个 3,686,400 B 的裸像素数组。整套 1280_800 样式表在本工程里
         *      从没被引用过，所以它一直被 `--gc-sections` 回收着 —— 只要一引用这套样式表，
         *      就会被拉回来。2026-10-16 实测：app 二进制 **7.31 MB → 11.00 MB**，
         *      而 app 分区只有 8 MB ⇒ `Error: app partition is too small ... (overflow 0x27e020)`。
         *      定位手法：`riscv32-esp-elf-nm --print-size --size-sort --radix=d <elf> | tail`
         *      ⇒ 直接看到那一个符号就是全部增量。
         *   ② **比例也不对**：它是 **720x720 的方图**，铺在 1280x800 上本来就要拉伸/裁切。
         * ⇒ 与我们在用的另外三套（`800_1280` / `720_1280` / `default`）保持一致，全部为 NULL
         *   （它们在同一行的同一个字段上都是 NULL）。桌面只剩 `.color` 的纯色底，观感一致。
         * 这让我们在**这一个文件**上与上游分岔 —— 记进"我们改过的非自研文件"清单。
         *    （这是工程里的框架副本，与上游原始版本分开维护。） */
        .wallpaper_image_resource = NULL,
    },
    .text = {
        .default_fonts_num = 21,
        .default_fonts = {
            gui::StyleFont::CUSTOM_SIZE(8, &esp_brookesia_font_maison_neue_book_8),
            gui::StyleFont::CUSTOM_SIZE(10, &esp_brookesia_font_maison_neue_book_10),
            gui::StyleFont::CUSTOM_SIZE(12, &esp_brookesia_font_maison_neue_book_12),
            gui::StyleFont::CUSTOM_SIZE(14, &esp_brookesia_font_maison_neue_book_14),
            gui::StyleFont::CUSTOM_SIZE(16, &esp_brookesia_font_maison_neue_book_16),
            gui::StyleFont::CUSTOM_SIZE(18, &esp_brookesia_font_maison_neue_book_18),
            gui::StyleFont::CUSTOM_SIZE(20, &esp_brookesia_font_maison_neue_book_20),
            gui::StyleFont::CUSTOM_SIZE(22, &esp_brookesia_font_maison_neue_book_22),
            gui::StyleFont::CUSTOM_SIZE(24, &esp_brookesia_font_maison_neue_book_24),
            gui::StyleFont::CUSTOM_SIZE(26, &esp_brookesia_font_maison_neue_book_26),
            gui::StyleFont::CUSTOM_SIZE(28, &esp_brookesia_font_maison_neue_book_28),
            gui::StyleFont::CUSTOM_SIZE(30, &esp_brookesia_font_maison_neue_book_30),
            gui::StyleFont::CUSTOM_SIZE(32, &esp_brookesia_font_maison_neue_book_32),
            gui::StyleFont::CUSTOM_SIZE(34, &esp_brookesia_font_maison_neue_book_34),
            gui::StyleFont::CUSTOM_SIZE(36, &esp_brookesia_font_maison_neue_book_36),
            gui::StyleFont::CUSTOM_SIZE(38, &esp_brookesia_font_maison_neue_book_38),
            gui::StyleFont::CUSTOM_SIZE(40, &esp_brookesia_font_maison_neue_book_40),
            gui::StyleFont::CUSTOM_SIZE(42, &esp_brookesia_font_maison_neue_book_42),
            gui::StyleFont::CUSTOM_SIZE(44, &esp_brookesia_font_maison_neue_book_44),
            gui::StyleFont::CUSTOM_SIZE(46, &esp_brookesia_font_maison_neue_book_46),
            gui::StyleFont::CUSTOM_SIZE(48, &esp_brookesia_font_maison_neue_book_48),
        },
    },
    .container = {
        .styles = {
            { .outline_width = 1, .outline_color = gui::StyleColor::COLOR(0xeb3b5a), },
            { .outline_width = 2, .outline_color = gui::StyleColor::COLOR(0xfa8231), },
            { .outline_width = 3, .outline_color = gui::StyleColor::COLOR(0xf7b731), },
            { .outline_width = 2, .outline_color = gui::StyleColor::COLOR(0x20bf6b), },
            { .outline_width = 1, .outline_color = gui::StyleColor::COLOR(0x0fb9b1), },
            { .outline_width = 3, .outline_color = gui::StyleColor::COLOR(0x2d98da), },
        },
    },
};

constexpr base::Manager::Data STYLESHEET_1280_800_DARK_CORE_MANAGER_DATA = {
    .app = {
        .max_running_num = 3,
    },
    .flags = {
        .enable_app_save_snapshot = 1,
    },
};

constexpr const char *STYLESHEET_1280_800_DARK_CORE_INFO_DATA_NAME = "1280x800 Dark";

constexpr base::Context::Data STYLESHEET_1280_800_DARK_CORE_DATA = {
    .name = STYLESHEET_1280_800_DARK_CORE_INFO_DATA_NAME,
    .screen_size = gui::StyleSize::RECT(1280, 800),
    .display = STYLESHEET_1280_800_DARK_CORE_DISPLAY_DATA,
    .manager = STYLESHEET_1280_800_DARK_CORE_MANAGER_DATA,
};

} // namespace esp_brookesia::systems::phone
