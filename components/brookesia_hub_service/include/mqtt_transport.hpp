/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 真正的传输层：TLS **双向认证** + MQTT，连 Pi 上的 broker。
 *
 * 契约见 `docs/mqtt-contract.md`；Pi 侧的实况回执在同一文件第 10 节。
 * 与 `LoopbackTransport` 实现同一个 `ChatTransport` 接口 ⇒ Hub 的 UI 一行不用改。
 *
 * 几条硬要求（照抄契约，别"优化"）：
 *   - **QoS 1、不要 retained**（`hub/chat/…` 两个主题）。设成 retained 会让 P4 一开机就重放旧回复。
 *   - `hub/device/p4/status` **要** retained，并在 LWT 里放 offline 版本。
 *   - **断连时不排队**：`send()` 在上线前直接返回 false，不让 esp-mqtt 的 outbox 攒着，
 *     恢复后一次性推出去（参考项目踩过这个坑）。
 *   - 回调里**绝不碰 LVGL**：只把 `Reply` 塞进调用方给的 handler（Hub 里是加锁队列）。
 *
 * 证书是**编译期嵌入**的（`tools/gen_certs_header.py` → `hub_certs.h`），
 *    源文件在工程根的 `secrets/` 目录，**已被 .gitignore 挡住**。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_timer.h"
#include "mqtt_client.h"

#include "chat_transport.hpp"
#include "hub_config.hpp"

namespace esp_brookesia::apps::chat {

/** 连接状态。UI 每隔一小会儿读一次（`atomic`，不加锁）。 */
enum class MqttState : int {
    Idle = 0,     /* 还没 start()，或者 stop() 过 */
    Connecting,   /* 客户端已启动，正在连 / 掉线自动重连中 */
    Connected,
    Failed,       /* 启动失败（缺证书 / 配置非法 / 建客户端失败） */
};

class MqttTransport : public ChatTransport {
public:
    /** 反控命令的回调。**跑在 esp-mqtt 任务上下文** ⇒ 只许入队，绝不碰 LVGL。 */
    using CommandHandler = std::function<void(const Command &)>;

    MqttTransport() = default;
    ~MqttTransport() override;

    MqttTransport(const MqttTransport &) = delete;
    MqttTransport &operator=(const MqttTransport &) = delete;

    /** 编译期有没有嵌入证书（没有就必然连不上，UI 要如实说）。 */
    static bool certsPresent(void);

    /**
     * 建客户端并启动。**幂等**：已经在跑就先 `stop()`。
     * @return true = 客户端已启动（**不代表已连上**，连接是异步的，看 `state()`）
     */
    bool start(const BrokerConfig &cfg);

    void stop(void);

    MqttState state(void) const
    {
        return _state.load(std::memory_order_acquire);
    }

    bool isConnected(void) const
    {
        return state() == MqttState::Connected;
    }

    void setReplyHandler(ReplyHandler handler) override;
    bool send(const std::string &text, std::string &id) override;
    const char *name(void) const override
    {
        return "mqtt";
    }

    /** 注册反控命令回调（单槽）。不注册 ⇒ 命令照收、只打日志、不回 ack。 */
    void setCommandHandler(CommandHandler handler);

    /**
     * 回一条执行结果到 `hub/command/p4/ack`。**没连上就是 false**（和 send 一样不排队）。
     * @param error 非空 ⇒ `ok` 会被强制写成 false，并把原因带上
     * @param closed ≥0 ⇒ 带一个 `"closed": N` 字段（只有 close_all 用得上：
     *               告诉 Pi"这一下清掉了几个 app"）。默认 −1 = 不带这个字段。
     */
    bool publishAck(const Command &cmd, bool ok, const char *error = nullptr, int closed = -1);

private:
    static void _onEvent(void *args, esp_event_base_t base, int32_t event_id, void *event_data);

    void _handleConnected(void);
    void _handleDisconnected(void);
    void _handleData(const esp_mqtt_event_handle_t event);
    void _handleCommand(const esp_mqtt_event_handle_t event);
    void _handleError(const esp_mqtt_event_handle_t event);

    /** 上线/下线状态。retained，QoS 1。 */
    void _publishStatus(bool online);

    esp_mqtt_client_handle_t _client = nullptr;
    BrokerConfig             _cfg;
    std::atomic<MqttState>   _state{MqttState::Idle};

    std::mutex   _handler_mutex;
    ReplyHandler _handler;

    /* 反控命令用**另一把锁**：它和回执的生命周期、调用者都不同（回执由 app 注册，
     * 命令由常驻的 HubCommands 注册），共用一把只会让"某个没注册"变成"两个都没了"。 */
    std::mutex     _cmd_mutex;
    CommandHandler _cmd_handler;

    std::string _client_id;
    std::string _status_payload;
    unsigned    _counter = 0;
    uint32_t    _boot_id = 0;

    /** 每 30 s 打一次堆水位。连上才跑；用于"小智同时在线"与"2 小时有没有下降"。 */
    esp_timer_handle_t _heap_timer = nullptr;
};

} // namespace esp_brookesia::apps::chat
