/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Hub:Service"
#include "esp_lib_utils.h"

#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_netif.h"   /* IP_EVENT / IP_EVENT_STA_GOT_IP */

#include "brookesia_hub_service.hpp"
#include "brookesia_net.hpp"

namespace esp_brookesia::services {

namespace {

/* 未读环形缓冲上限（计划 §7 定的是 32 条）。app 两天没开也不会把 RAM 吃穿。 */
constexpr size_t UNREAD_MAX = 32;

/* 还在流式接收中的 id 上限。Pi 侧每个 id 都会给 final，这个单纯是防病态情况。 */
constexpr size_t PARTIAL_MAX = 32;

/* 开机时等 WiFi 拿 IP 的上限。等不到也照样建客户端 —— esp-mqtt 自己会重试，
 * 而这正是"断网 30 分钟再恢复"要靠的那条路。 */
constexpr uint32_t BOOT_WAIT_MS = 60 * 1000;

/* 短命任务：只做 waitConnected + esp_mqtt_client_stop/start。
 * 4 KB 够用（esp-mqtt 的活在自己的任务里干）；**别调大** —— 内部 RAM 很紧（这个数是量出来的，不是拍的）。 */
constexpr uint32_t TASK_STACK = 4096;
constexpr UBaseType_t TASK_PRIO = 4;

} // namespace

HubService &HubService::getInstance()
{
    static HubService instance;
    return instance;
}

bool HubService::certsPresent(void)
{
    return chat::MqttTransport::certsPresent();
}

bool HubService::isConfigured(void) const
{
    std::lock_guard<std::mutex> lock(_cfg_mutex);
    return _cfg.isConfigured();
}

chat::BrokerConfig HubService::config(void) const
{
    std::lock_guard<std::mutex> lock(_cfg_mutex);
    return _cfg;
}

bool HubService::send(const std::string &text, std::string &id)
{
    if (!_started) {
        return false;
    }

    /* 和 `_bringUp()` 用同一把锁：那边会 `esp_mqtt_client_destroy()`，而
     * `send()` 读的是 `_client` 这个裸指针 —— 不加锁就是"发消息正好撞上改配置" ⇒
     * 用后释放。**用 try_lock**：重建期间（最多等到 WiFi 的 60 s）就别让 LVGL 任务
     * 在这里干等，直接告诉用户"正在重连"。 */
    std::unique_lock<std::mutex> lock(_op_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        ESP_UTILS_LOGW("正在重建 MQTT 连接，本次发送放弃");
        return false;
    }
    return _mqtt.send(text, id);
}

void HubService::setCommandHandler(chat::MqttTransport::CommandHandler handler)
{
    std::lock_guard<std::mutex> lock(_cmd_mutex);
    _cmd_handler = std::move(handler);
}

bool HubService::publishAck(const chat::Command &cmd, bool ok, const char *error, int closed)
{
    if (!_started) {
        return false;
    }

    /* 和 `send()` 同一把锁、同一个理由：`_bringUp()` 可能正在 `esp_mqtt_client_destroy()`。
     * 拿不到锁就放弃这条 ack —— 重建期间的 ack 本来也没有意义。 */
    std::unique_lock<std::mutex> lock(_op_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        ESP_UTILS_LOGW("正在重建 MQTT 连接，本次 ack 放弃（id=%s cmd=%s）", cmd.id.c_str(), cmd.cmd.c_str());
        return false;
    }
    return _mqtt.publishAck(cmd, ok, error, closed);
}

void HubService::_onTransportCommand(const chat::Command &cmd)
{
    /* 跑在 esp-mqtt 任务里 ⇒ 只取快照、只回调，绝不碰 LVGL（契约见头文件）。 */
    chat::MqttTransport::CommandHandler handler;
    {
        std::lock_guard<std::mutex> lock(_cmd_mutex);
        handler = _cmd_handler;
    }
    if (handler) {
        handler(cmd);
    }
}

void HubService::setReplyHandler(ReplyHandler handler)
{
    std::lock_guard<std::mutex> lock(_reply_mutex);
    _handler = std::move(handler);
}

std::vector<ChatReply> HubService::takeUnread(void)
{
    std::vector<ChatReply> out;
    std::lock_guard<std::mutex> lock(_reply_mutex);
    out.swap(_unread);
    return out;
}

/* ==========================================================================
 * 启动 / 重建
 * ========================================================================== */

/* 拿到 IP 事件：只在"服务已启动、客户端还从没建过"时补一次。
 * 用**自己的** esp_event 处理器，不占用 `NetService::setStateCallback` 那个单槽
 * （借别人的单槽回调之前，先问还有谁在用同一个）。 */
void HubService::_onIpEvent(void *arg, esp_event_base_t, int32_t, void *)
{
    auto *self = static_cast<HubService *>(arg);
    if (self != nullptr) {
        self->_onNetworkUp();
    }
}

void HubService::_onNetworkUp(void)
{
    if (!_started) {
        return;
    }
    /* 已经连上 / 正在连 / 建失败过 ⇒ 别插手。后者重试也没意义（缺证书之类）。
     * 断线重连由 esp-mqtt 自己负责，不经过这里。 */
    if (_mqtt.state() != chat::MqttState::Idle) {
        return;
    }
    if (_bringups.load() > 0) {
        return;
    }
    ++_bringups;
    if (xTaskCreate(&HubService::_netUpTask, "hub_netup", TASK_STACK, this, TASK_PRIO, nullptr) != pdPASS) {
        --_bringups;
        ESP_UTILS_LOGW("建任务失败，本次上网事件没能拉起 MQTT");
    }
}

bool HubService::start(void)
{
    if (_started) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(_cfg_mutex);
        _cfg = chat::configLoad();
    }
    _started = true;

    const chat::BrokerConfig cfg = config();
    ESP_UTILS_LOGI("Hub 服务启动：%s", cfg.isConfigured() ? cfg.summary().c_str() : "未配置树莓派");

    /* 以后用户可能在「设置 → WLAN」里把 WiFi 打开 —— 那条路不会再调 start()，
     * 所以这里挂一个自己的 IP 事件处理器兜底。 */
    static bool hooks = false;
    if (!hooks) {
        const esp_err_t err = esp_event_handler_register(
                                  IP_EVENT, IP_EVENT_STA_GOT_IP, &HubService::_onIpEvent, this);
        if (err != ESP_OK) {
            ESP_UTILS_LOGW("注册 IP 事件失败(%s)：WiFi 后开的话 MQTT 要等重启",
                           esp_err_to_name(err));
        }
        hooks = (err == ESP_OK);
    }

    if (!cfg.isConfigured()) {
        ESP_UTILS_LOGW("没有配置树莓派 ⇒ 不建 MQTT 客户端（Hub 里点 ⚙ 设置填一下）");
        return false;
    }
    if (!chat::MqttTransport::certsPresent()) {
        ESP_UTILS_LOGE("缺证书：secrets/ 下没有 ca.crt / p4.crt / p4.key，编进去的是空的");
        return false;
    }
    if (!NetService::getInstance().isStarted()) {
        ESP_UTILS_LOGW("WiFi 当前是关的（NVS storage/wifi_en=0 或初始化失败）"
                       "⇒ 先不连 MQTT，等它起来拿到 IP 再连");
        return false;
    }

    ++_bringups;
    if (xTaskCreate(&HubService::_bootTask, "hub_boot", TASK_STACK, this, TASK_PRIO, nullptr) != pdPASS) {
        --_bringups;
        ESP_UTILS_LOGE("建启动任务失败 ⇒ MQTT 没起来");
        return false;
    }
    return true;
}

bool HubService::restart(void)
{
    {
        std::lock_guard<std::mutex> lock(_cfg_mutex);
        _cfg = chat::configLoad();
    }

    ++_bringups;
    if (xTaskCreate(&HubService::_restartTask, "hub_restart", TASK_STACK, this, TASK_PRIO, nullptr) != pdPASS) {
        --_bringups;
        ESP_UTILS_LOGE("改配置后重建连接失败：建任务失败");
        return false;
    }
    return true;
}

void HubService::stop(void)
{
    /* `esp_mqtt_client_stop()` 可能阻塞到秒级 ⇒ 不能为了 stop() 去排队等一个可能
     * 正在等 WiFi（最多 60 s）的任务。拿不到锁就算了。 */
    if (!_op_mutex.try_lock()) {
        ESP_UTILS_LOGW("正在连接/重建中，stop() 跳过");
        return;
    }
    std::lock_guard<std::mutex> lock(_op_mutex, std::adopt_lock);
    _mqtt.stop();
}

void HubService::_bringUp(void)
{
    std::lock_guard<std::mutex> lock(_op_mutex);

    const chat::BrokerConfig cfg = config();

    /* 先把旧的拆掉（改配置那条路会走到这里）。stop() 是幂等的。 */
    _mqtt.stop();

    if (!cfg.isConfigured() || !chat::MqttTransport::certsPresent()) {
        return;
    }

    /* `waitConnected()` 在 WiFi 服务根本没起来时会**立刻**返回 false ——
     *    别把它记成"等了 60 秒超时"（那是个假数字）。 */
    const bool net_on = NetService::getInstance().isStarted();
    const bool wifi_up = net_on && NetService::getInstance().waitConnected(BOOT_WAIT_MS);
    if (!wifi_up) {
        if (net_on) {
            ESP_UTILS_LOGW("等 WiFi 拿到 IP 超时（%u ms）—— 仍然先建客户端，esp-mqtt 会自己重试",
                           static_cast<unsigned>(BOOT_WAIT_MS));
        } else {
            ESP_UTILS_LOGW("WiFi 服务没起来（wifi_en=0？）—— 仍然先建客户端，"
                           "等它后来连上再由 IP 事件补");
        }
    }

    _mqtt.setReplyHandler([this](const chat::Reply &reply) {
        _onTransportReply(reply);
    });

    /* 命令处理器跟着每次（重）建客户端都挂一遍 —— 和回执一样，别指望"上一次设过了"。
     * lambda 每次都去读当前的 `_cmd_handler`，所以后来换处理器也立刻生效。 */
    _mqtt.setCommandHandler([this](const chat::Command &cmd) {
        _onTransportCommand(cmd);
    });

    if (_mqtt.start(cfg)) {
        ESP_UTILS_LOGI("MQTT 客户端已启动（%s，WiFi %s）",
                       cfg.summary().c_str(), wifi_up ? "已在" : "未就绪");
    }
}

void HubService::_bootTask(void *arg)
{
    static_cast<HubService *>(arg)->_bringUp();
    --static_cast<HubService *>(arg)->_bringups;
    vTaskDelete(nullptr);
}

void HubService::_restartTask(void *arg)
{
    static_cast<HubService *>(arg)->_bringUp();
    --static_cast<HubService *>(arg)->_bringups;
    vTaskDelete(nullptr);
}

void HubService::_netUpTask(void *arg)
{
    static_cast<HubService *>(arg)->_bringUp();
    --static_cast<HubService *>(arg)->_bringups;
    vTaskDelete(nullptr);
}

/* ==========================================================================
 * 回执：累积全文 → 投递给 app，或塞进未读
 * ========================================================================== */

void HubService::_onTransportReply(const chat::Reply &reply)
{
    ChatReply snapshot;
    ReplyHandler handler;
    bool deliver = false;

    {
        std::lock_guard<std::mutex> lock(_reply_mutex);

        if (reply.id.empty()) {
            return;
        }

        ChatReply *slot = nullptr;
        for (auto &item : _partials) {
            if (item.id == reply.id) {
                slot = &item;
                break;
            }
        }
        if (slot == nullptr) {
            if (_partials.size() >= PARTIAL_MAX) {
                ESP_UTILS_LOGW("同时在接收的回执超过 %u 条，丢掉最旧的一条", static_cast<unsigned>(PARTIAL_MAX));
                _partials.erase(_partials.begin());
            }
            _partials.push_back(ChatReply{});
            slot = &_partials.back();
            slot->id = reply.id;
        }

        slot->text += reply.delta;
        slot->seq = reply.seq;
        slot->final = reply.final;
        if (!reply.error.empty()) {
            slot->error = reply.error;
        }

        snapshot = *slot;
        handler = _handler;
        deliver = static_cast<bool>(handler);

        if (reply.final) {
            if (!deliver) {
                /* app 没开：攒起来，下次进 app 时补写进历史（环形，满了丢最旧）。 */
                if (_unread.size() >= UNREAD_MAX) {
                    _unread.erase(_unread.begin());
                    ESP_UTILS_LOGW("未读回执超过 %u 条，丢掉最旧的一条", static_cast<unsigned>(UNREAD_MAX));
                }
                _unread.push_back(snapshot);
                ESP_UTILS_LOGI("回执完成（app 未打开，存为未读）id=%s %u 字",
                               snapshot.id.c_str(), static_cast<unsigned>(snapshot.text.size()));
            }
        }

        if (reply.final) {
            /* 从累积表里删掉，内存不随对话增长。 */
            for (size_t i = 0; i < _partials.size(); ++i) {
                if (_partials[i].id == reply.id) {
                    _partials.erase(_partials.begin() +
                                    static_cast<std::vector<ChatReply>::difference_type>(i));
                    break;
                }
            }
        }
    }

    if (deliver) {
        /* 回调可能在 mqtt 任务上下文 —— 调用方只许塞队列，绝不碰 LVGL。 */
        handler(snapshot);
    }
}

} // namespace esp_brookesia::services
