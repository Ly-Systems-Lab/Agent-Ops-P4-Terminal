/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hub 的常驻服务 —— **与 app 的生命周期无关**。
 *
 * 早些时候 MQTT 客户端是 `Hub` 的成员：关掉 app 就断线，app 没打开时收到的
 * 回复直接丢掉。现在把它搬到服务里，开机就连、关 app 也不断：
 *
 *   开机 → NetService 起 WiFi → HubService 等 IP（最多 60 s）→ MQTT + TLS 连上
 *   进 Hub app → 只注册一个回执回调，读写服务里的状态，**不启停任何东西**
 *   关 Hub app → 回调置空；这段时间到达的回复进「未读」环形缓冲（32 条），
 *               下次进 app 时补写进历史
 *
 * 分工与 `brookesia_web_console`（服务）+ `brookesia_app_webconsole`（入口）一致。
 *
 * 线程约定：
 *   - `send()` 由 **LVGL 任务**调用；`ReplyHandler` 由 **esp-mqtt 任务**调用；
 *   - `takeUnread()` / `setReplyHandler()` / `state()` 任何任务都能调；
 *   - `restart()` 会起一个短命任务去做 `stop()` + 重建 —— 因为
 *     `esp_mqtt_client_stop()` **可能阻塞到秒级**，绝不能在 LVGL 任务里做。
 *
 * 断连语义不变：`send()` 在没连上时**直接返回 false、不排队**（契约 §5.3）。
 */
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "esp_event.h"

#include "chat_transport.hpp"
#include "hub_config.hpp"
#include "mqtt_transport.hpp"

namespace esp_brookesia::services {

/* 传输层那些类型（`Reply` / `MqttState` / `BrokerConfig` …）还在 `apps::chat` 里 ——
 * 历史原因，见 `chat_transport.hpp` 顶部的注释。这里起个别名，省得每处都写 `apps::chat::`。
 * 别名只在 `services` 里可见，不会改变 app 侧 `chat::X` 的含义。 */
namespace chat = esp_brookesia::apps::chat;

/**
 * 一条回执的**快照**（累积全文，不是本片增量）。
 *
 * 为什么给快照而不是增量：app 可能在中途才打开（关着的时候回复已经在流了）。
 * 给增量的话，前半截文本已经丢了，app 只能显示半句话；给全文则任何时候接手都对得上。
 */
struct ChatReply {
    std::string id;
    std::string text;    /* 到目前为止累积的全文 */
    int         seq = 0;
    bool        final = false;
    std::string error;   /* 非空 ⇒ 错误回执，text 不再有意义 */
};

class HubService {
public:
    using ReplyHandler = std::function<void(const ChatReply &)>;

    static HubService &getInstance();

    HubService(const HubService &) = delete;
    HubService &operator=(const HubService &) = delete;

    /**
     * 开机调用一次。**幂等**。
     *
     * @return true  = 已经（或正在）把 MQTT 拉起来。**不代表已连上**，看 `state()`
     *         false = 没配 Pi / 缺证书 / WiFi 没起来 ⇒ 服务就绪但不建客户端
     *
     * 内部会起一个短命任务等 WiFi 拿到 IP，**不会阻塞调用方**。
     */
    bool start(void);

    /**
     * 配置改过之后重建连接（app 保存设置时调）。**不阻塞调用方**。
     * @return true = 已经排上重建；false = 任务创建失败
     */
    bool restart(void);

    void stop(void);

    bool isStarted(void) const
    {
        return _started.load(std::memory_order_acquire);
    }

    /** NVS 里有没有可用的 broker 地址（host 非空）。 */
    bool isConfigured(void) const;

    /** 编译期有没有嵌入证书。没有就必然连不上，UI 要如实说。 */
    static bool certsPresent(void);

    chat::MqttState state(void) const
    {
        return _mqtt.state();
    }

    bool isConnected(void) const
    {
        return _mqtt.isConnected();
    }

    /** 当前配置的副本（UI 展示用；密码也在里面，**别打日志**）。 */
    chat::BrokerConfig config(void) const;

    /**
     * 发一条用户消息。**没连上就是 false，不排队**（契约 §5.3）。
     * 成功时把本次的 id 写进 `id`，回执会带着它回来。
     */
    bool send(const std::string &text, std::string &id);

    /**
     * 注册反控命令回调（单槽）。**常驻**：app 开没开都有效（`HubCommands` 在开机时注册）。
     * handler 跑在 **esp-mqtt 任务上下文** ⇒ 只许入队，绝不碰 LVGL。
     */
    void setCommandHandler(chat::MqttTransport::CommandHandler handler);

    /**
     * 回一条命令执行结果到 `hub/command/p4/ack`。**没连上就是 false，不排队**。
     * 由 LVGL 任务在命令执行完之后调（`esp_mqtt_client_publish` 只是塞 outbox，不阻塞）。
     */
    bool publishAck(const chat::Command &cmd, bool ok, const char *error = nullptr, int closed = -1);

    /**
     * 注册回执回调（单槽，后设的覆盖前面的）。app 在 `run()` 里设、退出时传
     * `nullptr` 清掉。
     * handler 跑在 **esp-mqtt 任务上下文** ⇒ 只许塞队列，绝不碰 LVGL。
     */
    void setReplyHandler(ReplyHandler handler);

    /**
     * 取走「app 没开时到达、且已经 final」的回复，按完成顺序；**取走即清空**。
     * app 每次 `run()` 调一次，补写进历史。
     */
    std::vector<ChatReply> takeUnread(void);

private:
    HubService() = default;
    ~HubService() = default;

    static void _bootTask(void *arg);
    static void _restartTask(void *arg);
    static void _netUpTask(void *arg);

    /** 拿到 IP 时补一次连接（开机时 WiFi 是关的、后来才打开的那条路）。
     * 跑在 esp_event 任务里 ⇒ 只能计数/建任务，**不能阻塞**。 */
    static void _onIpEvent(void *arg, esp_event_base_t base, int32_t event_id, void *data);
    void _onNetworkUp(void);

    /** 真正干活的那一段：等 IP → 拆旧客户端 → 建新客户端。**会阻塞**，只许在任务里跑。 */
    void _bringUp(void);

    /** esp-mqtt 任务里被调用：累积全文 → 投递给 app 或塞进未读。 */
    void _onTransportReply(const chat::Reply &reply);

    /** esp-mqtt 任务里被调用：把命令转给注册者（**不在这里执行**）。 */
    void _onTransportCommand(const chat::Command &cmd);

    chat::MqttTransport _mqtt;

    mutable std::mutex  _cfg_mutex;
    chat::BrokerConfig  _cfg;

    std::mutex  _op_mutex;      /* 串行化 _bringUp（开机那个和改配置那个别打架） */
    std::atomic<bool> _started{false};
    std::atomic<int>  _bringups{0};   /* 已经排上队的 _bringUp 任务数 */

    /* 回执累积 + 未读，共用一把锁（都是小操作，没必要拆）。 */
    std::mutex             _reply_mutex;
    ReplyHandler           _handler;
    std::vector<ChatReply> _partials;   /* 还在流式接收中的，按 id 线性找 */
    std::vector<ChatReply> _unread;     /* app 没开时已完成的 */

    /* 反控命令的回调。和 `_handler` 分开：app 关掉会把 `_handler` 清空，
     * 而命令处理是**常驻**的，不受 app 开关影响。 */
    std::mutex                         _cmd_mutex;
    chat::MqttTransport::CommandHandler _cmd_handler;
};

} // namespace esp_brookesia::services
