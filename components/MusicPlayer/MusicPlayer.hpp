/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "file_iterator.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * @brief MusicPlayer application for displaying the app name on the device screen
 *
 */
class MusicPlayer: public systems::phone::App {
public:
    /**
     * @brief Get the singleton instance of MusicPlayer
     *
     * @param use_status_bar Show status bar
     * @param use_navigation_bar Show navigation bar
     * @return MusicPlayer* Singleton instance pointer
     */
    static MusicPlayer *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    /**
     * @brief 打开 app 之后开始播第 1 首（给 MQTT 的 `music` 命令用）
     *
     * 等价于用户点了列表里第一行：内部走 `lv_demo_music_play(0)`
     * （`track_load()` + `resume()`）。
     *
     * @return true  = 播放**请求**已发出（真的出声看下一行 demo 日志：
     *                 `Failed to start track` 就是没放起来）
     *         false = UI 还没建 / 曲目列表为空 ⇒ 什么都没做
     */
    bool playFirst(void);

    /**
     * @brief Destroy the MusicPlayer object
     *
     */
    ~MusicPlayer();

protected:
    /**
     * @brief Construct a new MusicPlayer object (private to enforce singleton)
     *
     * @param use_status_bar Show status bar
     * @param use_navigation_bar Show navigation bar
     */
    MusicPlayer(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool deinit(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    static MusicPlayer *_instance;
    file_iterator_instance_t *_file_iterator;
    bool _player_active = false;
    bool _ui_active = false;
};

} // namespace esp_brookesia::apps