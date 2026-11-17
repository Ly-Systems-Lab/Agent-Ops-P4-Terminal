/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 反控命令的**执行器** —— 把 `hub/command/p4` 上收到的字符串变成屏幕上的动作。
 *
 * 当前支持（详见 `docs/mqtt-contract.md` 第 11 节）：
 *   `music`（play / open / close）、`aichat`（open / close）、**`close_all`**（一键清后台）。
 *
 * 为什么单独一个组件而不塞进 `HubService`：
 *   `HubService` 只认 MQTT（连 `brookesia_core` 的 UI 头都不该碰），而这里要
 *   `Context::sendAppEvent()` + `MusicPlayer::playFirst()`。混在一起会让服务组件
 *   依赖一个 app 组件，将来换 app 就得动传输层。
 *
 * 线程模型（这条是全部设计里最容易写错的地方）：
 *   - `_onMqttCommand` 跑在 **esp-mqtt 任务** ⇒ **只入队**，一个字都不碰 LVGL；
 *   - `_onPoll` 跑在 **LVGL 任务**（`lv_timer_handler` 里）⇒ 才允许 `sendAppEvent`。
 *   直接跨任务调 `sendAppEvent` 会写坏 LVGL 的全局事件链表（所以只能在 LVGL 任务里碰）。
 *
 * 这个 timer 是**开机时在 main.cpp 里建的**（不在任何 app 的 `run()` 里）⇒
 *   宿主框架的 `enable_recycle_resource` 不会去删它。
 *
 * 报文与 acks 见 `docs/mqtt-contract.md` 第 11 节。
 */
#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

#include "chat_transport.hpp"
#include "systems/base/esp_brookesia_base_context.hpp"

namespace esp_brookesia::services {

class HubCommands {
public:
    /** 命令队列深度。正常场景一次只有一条；这里只是挡住"Pi 连发"那一瞬间。 */
    static constexpr UBaseType_t QUEUE_LEN = 4;

    /** 轮询周期。100 ms ≈ 人感觉不到；队列空时只是一次 `xQueueReceive(…, 0)`。 */
    static constexpr uint32_t POLL_PERIOD_MS = 100;

    static HubCommands &getInstance();

    HubCommands(const HubCommands &) = delete;
    HubCommands &operator=(const HubCommands &) = delete;

    /**
     * 开机调用一次。**幂等**。
     *
     * **必须在 LVGL 锁内调用**（`main.cpp` 的 `LvLockGuard` 块里）——
     *    它要 `lv_timer_create`，那是往 LVGL 的全局链表插一项。
     *
     * @return true = 已就绪（命令会被执行）；false = 建立失败（原因已打日志）
     */
    bool begin(systems::base::Context *context);

private:
    /* 跨任务传递的**纯 POD**：不用 std::string，免得在两个任务的堆上互相引用。 */
    struct Pending {
        char id[40];    /* Pi 给的配对键，原样回给 ack */
        char cmd[16];   /* "music" / "aichat" */
        char arg[16];   /* 可选 */
    };

    HubCommands() = default;
    ~HubCommands() = default;

    /** esp-mqtt 任务上下文：只入队。
     * 这里写全名 `apps::chat::Command`：`chat` 那个别名定义在
     *     `brookesia_hub_service.hpp` 里，本头文件**只 include 传输层的头**，
     *     用别名会编不过（第一版就是这么错的）。 */
    static void _onMqttCommand(const apps::chat::Command &cmd);

    /** LVGL 任务上下文：出队 → 执行 → 回 ack。 */
    static void _onPoll(lv_timer_t *timer);

    void _execute(const Pending &pending);

    /** `close_all`：一键清理所有后台 —— 等价于最近任务里点那个垃圾桶。 */
    void _closeAll(const Pending &pending);

    /** 按 app **名字**找 id。id 是安装顺序发的号（`_app_free_id++`），绝不能写死。 */
    int _findAppId(const char *name) const;

    /** @param closed ≥0 ⇒ ack 里带 `"closed": N`（只有 close_all 用；默认不带） */
    void _ack(const Pending &pending, bool ok, const char *error, int closed = -1);

    systems::base::Context *_context = nullptr;
    QueueHandle_t          _queue = nullptr;
    lv_timer_t            *_timer = nullptr;
};

} // namespace esp_brookesia::services