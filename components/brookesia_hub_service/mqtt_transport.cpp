/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Hub:Mqtt"
#include "esp_lib_utils.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"

#include "cJSON.h"

#include "hub_certs.h"
#include "mqtt_transport.hpp"

namespace esp_brookesia::apps::chat {

namespace {

constexpr const char *TOPIC_CHAT = "hub/chat/p4";
constexpr const char *TOPIC_REPLY = "hub/chat/p4/reply";
constexpr const char *TOPIC_STATUS = "hub/device/p4/status";

/* 反控（Pi → P4）。契约 §4 早就把这个主题占着了，这里才第一次真的用。
 * Pi 侧的 mosquitto ACL 必须同时放开两条，否则 P4 根本收不到 / 回不去：
 *      topic read  hub/command/p4        （p4 账号）
 *      topic write hub/command/p4/ack    （p4 账号）
 *    没放开时 MQTT 3.1.1 的订阅会被拒 —— 表现是**反复断连且没有明确提示**。 */
constexpr const char *TOPIC_COMMAND = "hub/command/p4";
constexpr const char *TOPIC_ACK = "hub/command/p4/ack";

/* 收发缓冲各 8 KB —— 契约要求单条 ≤ 4 KB，留一倍余量。
 * esp-mqtt 的默认值是 **1024 B，超过就整条丢弃、只打一行 warn**。 */
constexpr int MQTT_BUFFER_BYTES = 8 * 1024;

/* 任务栈默认 6144。回调里要跑 cJSON，给到 8 KB 更稳（别只写"我猜够用"，
 * 连上之后会把余量打出来）。 */
constexpr int MQTT_TASK_STACK = 8 * 1024;

/* 单条上限（协议层）。比契约的 4 KB 多留一点，真正拦人是 8 KB 的缓冲。 */
constexpr int MQTT_MAX_PAYLOAD = 4096;

/* 堆水位的采样周期。120 s 太长看不到瞬时变化，10 s 又太吵 —— 30 s 是两者的折中。
 * 它同时也是「2 小时 free 无下降」的唯一证据来源。 */
constexpr uint64_t HEAP_LOG_PERIOD_US = 30ULL * 1000 * 1000;

} // namespace

MqttTransport::~MqttTransport()
{
    stop();
}

bool MqttTransport::certsPresent(void)
{
    return HUB_CERTS_PRESENT != 0;
}

void MqttTransport::setReplyHandler(ReplyHandler handler)
{
    std::lock_guard<std::mutex> lock(_handler_mutex);
    _handler = std::move(handler);
}

void MqttTransport::setCommandHandler(CommandHandler handler)
{
    std::lock_guard<std::mutex> lock(_cmd_mutex);
    _cmd_handler = std::move(handler);
}

static void logHeap(const char *when)
{
    const size_t in_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t in_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_UTILS_LOGI("%s 内部 free/largest=%u/%u KB   DMA free/largest=%u/%u KB   PSRAM free/largest=%u/%u KB",
                   when,
                   (unsigned)(in_free / 1024), (unsigned)(in_largest / 1024),
                   (unsigned)(dma_free / 1024), (unsigned)(dma_largest / 1024),
                   (unsigned)(ps_free / 1024), (unsigned)(ps_largest / 1024));
}

void heapTimerCallback(void *)
{
    logHeap("周期(30s)");
}

bool MqttTransport::start(const BrokerConfig &cfg)
{
    stop();

    if (!certsPresent()) {
        ESP_UTILS_LOGE("缺证书：secrets/ 下没有 ca.crt / p4.crt / p4.key，编进去的是空的");
        _state.store(MqttState::Failed);
        return false;
    }
    if (!cfg.isConfigured()) {
        ESP_UTILS_LOGW("没有配置 broker 地址，不启动");
        _state.store(MqttState::Idle);
        return false;
    }

    _cfg = cfg;

    /* client_id = p4-<基础 MAC 后 3 字节小写 hex>，稳定不变（Pi 侧可据此做白名单）。 */
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "p4-%02x%02x%02x", mac[3], mac[4], mac[5]);
    _client_id = id_buf;

    /* 每次启动换一个 boot_id：id 里带上它，P4 重启后不会和 Pi 的去重窗口撞车。 */
    _boot_id = esp_random();

    const std::string lwt =
        std::string("{\"v\":1,\"online\":false,\"client_id\":\"") + _client_id + "\"}";

    logHeap("MQTT 启动前");

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = _cfg.host.c_str();
    mqtt_cfg.broker.address.port = _cfg.port;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;

    mqtt_cfg.broker.verification.certificate = HUB_CA_CERT;
    mqtt_cfg.broker.verification.certificate_len = sizeof(HUB_CA_CERT);
    /* 必须跳过：Pi 的服务器证书 SAN 里写的是**会变的热点网段 IP**，
     *    而 CA 链仍然照验 —— Pi 侧的 agent 客户端也是这么做的。 */
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;

    mqtt_cfg.credentials.client_id = _client_id.c_str();
    mqtt_cfg.credentials.username = _cfg.user.c_str();
    mqtt_cfg.credentials.authentication.password = _cfg.pass.c_str();
    /* 双向认证：不带客户端证书会被 broker **直接断连且没有任何提示**。 */
    mqtt_cfg.credentials.authentication.certificate = HUB_CLIENT_CERT;
    mqtt_cfg.credentials.authentication.certificate_len = sizeof(HUB_CLIENT_CERT);
    mqtt_cfg.credentials.authentication.key = HUB_CLIENT_KEY;
    mqtt_cfg.credentials.authentication.key_len = sizeof(HUB_CLIENT_KEY);

    mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.session.disable_clean_session = false;   /* clean session = true */
    mqtt_cfg.session.last_will.topic = TOPIC_STATUS;
    mqtt_cfg.session.last_will.msg = lwt.c_str();
    mqtt_cfg.session.last_will.msg_len = static_cast<int>(lwt.size());
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    mqtt_cfg.network.timeout_ms = 10000;
    mqtt_cfg.network.reconnect_timeout_ms = 10000;

    mqtt_cfg.task.stack_size = MQTT_TASK_STACK;
    mqtt_cfg.buffer.size = MQTT_BUFFER_BYTES;
    mqtt_cfg.buffer.out_size = MQTT_BUFFER_BYTES;

    _client = esp_mqtt_client_init(&mqtt_cfg);
    if (_client == nullptr) {
        ESP_UTILS_LOGE("esp_mqtt_client_init 失败");
        _state.store(MqttState::Failed);
        return false;
    }

    esp_err_t err = esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, &MqttTransport::_onEvent, this);
    if (err != ESP_OK) {
        ESP_UTILS_LOGE("注册事件回调失败：%s", esp_err_to_name(err));
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        _state.store(MqttState::Failed);
        return false;
    }

    err = esp_mqtt_client_start(_client);
    if (err != ESP_OK) {
        ESP_UTILS_LOGE("esp_mqtt_client_start 失败：%s", esp_err_to_name(err));
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        _state.store(MqttState::Failed);
        return false;
    }

    /* 周期采样堆水位。连上之后才真正有用，但启动就开没坏处（第一次一分钟后）。 */
    {
        esp_timer_create_args_t args = {};
        args.callback = &heapTimerCallback;
        args.name = "hub_heap";
        args.dispatch_method = ESP_TIMER_TASK;
        args.skip_unhandled_events = true;
        if (esp_timer_create(&args, &_heap_timer) != ESP_OK) {
            _heap_timer = nullptr;
            ESP_UTILS_LOGW("堆水位定时器创建失败（不影响连接）");
        } else {
            esp_timer_start_periodic(_heap_timer, HEAP_LOG_PERIOD_US);
        }
    }

    _state.store(MqttState::Connecting);
    ESP_UTILS_LOGI("MQTT 已启动 → %s（TLS 双向认证，client_id=%s）",
                   _cfg.summary().c_str(), _client_id.c_str());
    return true;
}

void MqttTransport::stop(void)
{
    if (_heap_timer != nullptr) {
        esp_timer_stop(_heap_timer);
        esp_timer_delete(_heap_timer);
        _heap_timer = nullptr;
    }

    if (_client == nullptr) {
        _state.store(MqttState::Idle);
        return;
    }

    esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
    _state.store(MqttState::Idle);
    ESP_UTILS_LOGI("MQTT 已停止");
}

void MqttTransport::_onEvent(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    auto *self = static_cast<MqttTransport *>(args);
    if (self == nullptr) {
        return;
    }

    auto event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        self->_handleConnected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        self->_handleDisconnected();
        break;
    case MQTT_EVENT_DATA:
        self->_handleData(event);
        break;
    case MQTT_EVENT_ERROR:
        self->_handleError(event);
        break;
    default:
        break;
    }
}

void MqttTransport::_handleConnected(void)
{
    _state.store(MqttState::Connected, std::memory_order_release);
    ESP_UTILS_LOGI("MQTT 已连接");

    if (_client != nullptr) {
        /* 两个订阅：回执（聊天）+ 命令（反控）。ACL 也只放开这两条。 */
        esp_mqtt_client_subscribe(_client, TOPIC_REPLY, 1);
        esp_mqtt_client_subscribe(_client, TOPIC_COMMAND, 1);
    }
    _publishStatus(true);

    logHeap("MQTT 连上后");
}

void MqttTransport::_handleDisconnected(void)
{
    /* esp-mqtt 会自动重连（10 s 一次），所以这里只是"掉线中"，不是失败。 */
    _state.store(MqttState::Connecting);
    ESP_UTILS_LOGW("MQTT 断开，等待自动重连");
}

void MqttTransport::_handleError(const esp_mqtt_event_handle_t event)
{
    if (event == nullptr || event->error_handle == nullptr) {
        return;
    }

    const esp_mqtt_error_codes_t *err = event->error_handle;
    if (err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        ESP_UTILS_LOGE("MQTT 传输层错误：tls=0x%x 栈=0x%x errno=%d",
                       err->esp_tls_last_esp_err, err->esp_tls_stack_err, err->esp_transport_sock_errno);
    } else if (err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        ESP_UTILS_LOGE("MQTT 连接被拒（认证/ACL？）return_code=0x%x", err->connect_return_code);
    } else {
        ESP_UTILS_LOGE("MQTT 错误 type=%d", static_cast<int>(err->error_type));
    }
}

void MqttTransport::_publishStatus(bool online)
{
    if (_client == nullptr) {
        return;
    }

    char buf[160];
    if (online) {
        snprintf(buf, sizeof(buf), "{\"v\":1,\"online\":true,\"client_id\":\"%s\"}", _client_id.c_str());
    } else {
        snprintf(buf, sizeof(buf), "{\"v\":1,\"online\":false,\"client_id\":\"%s\"}", _client_id.c_str());
    }

    /* retained：这样 Pi 重启后一订阅就知道 P4 在不在。 */
    esp_mqtt_client_publish(_client, TOPIC_STATUS, buf, 0, 1, 1);
}

void MqttTransport::_handleData(const esp_mqtt_event_handle_t event)
{
    if (event == nullptr || event->data == nullptr || event->data_len <= 0) {
        return;
    }

    /* 8 KB 缓冲 + ≤4 KB 报文 ⇒ 不应该分片。真分片了就是契约被破坏，直接丢并喊出来。 */
    if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
        ESP_UTILS_LOGE("收到分片报文（%d/%d），丢弃 —— 契约要求单条 ≤ 4 KB",
                       event->data_len, event->total_data_len);
        return;
    }
    if (event->data_len > MQTT_MAX_PAYLOAD) {
        ESP_UTILS_LOGE("报文 %d B 超过上限 %d B，丢弃", event->data_len, MQTT_MAX_PAYLOAD);
        return;
    }

    const std::string topic(event->topic, static_cast<size_t>(event->topic_len));
    if (topic == TOPIC_COMMAND) {
        _handleCommand(event);
        return;
    }
    if (topic != TOPIC_REPLY) {
        return;
    }

    /* payload 不是 NUL 结尾的，必须先拷进 std::string 再交给 cJSON。 */
    const std::string json(event->data, static_cast<size_t>(event->data_len));
    cJSON *root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_UTILS_LOGW("回执 JSON 解析失败（%d B）", event->data_len);
        return;
    }

    Reply reply;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    const cJSON *final_flag = cJSON_GetObjectItemCaseSensitive(root, "final");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");

    if (cJSON_IsString(id) && id->valuestring != nullptr) {
        reply.id = id->valuestring;
    }
    if (cJSON_IsNumber(seq)) {
        reply.seq = seq->valueint;
    }
    if (cJSON_IsString(delta) && delta->valuestring != nullptr) {
        reply.delta = delta->valuestring;
    }
    if (cJSON_IsBool(final_flag)) {
        reply.final = cJSON_IsTrue(final_flag);
    }

    if (cJSON_IsObject(error)) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(error, "msg");
        if (cJSON_IsString(code) && code->valuestring != nullptr) {
            reply.error = code->valuestring;
        } else {
            reply.error = "internal";
        }
        if (cJSON_IsString(msg) && msg->valuestring != nullptr) {
            reply.error += " ";
            reply.error += msg->valuestring;
        }
    }

    cJSON_Delete(root);

    if (reply.id.empty()) {
        ESP_UTILS_LOGW("回执没有 id，丢弃");
        return;
    }

    /* 逐帧打一行：这一阶段只有它能把"Pi 到底回没回"和"回了但 UI 没显示"分开。 */
    ESP_UTILS_LOGI("回执 id=%s seq=%d final=%d delta=%u B%s", reply.id.c_str(), reply.seq,
                   reply.final ? 1 : 0, static_cast<unsigned>(reply.delta.size()),
                   reply.error.empty() ? "" : "（error）");

    ReplyHandler handler;
    {
        std::lock_guard<std::mutex> lock(_handler_mutex);
        handler = _handler;
    }
    if (handler) {
        /* 回调可能在 MQTT 任务上下文 —— handler 只许塞队列，绝不碰 LVGL。 */
        handler(reply);
    }
}

/* 反控命令：**只解析、只回调**。开 app / 放音乐都是 LVGL 那边的事 ——
 * 这里连 `brookesia_core` 的 UI 头都不该碰（框架的全局链表只能在 LVGL 任务里动）。 */
void MqttTransport::_handleCommand(const esp_mqtt_event_handle_t event)
{
    if (event == nullptr || event->data == nullptr || event->data_len <= 0) {
        return;
    }

    /* payload 不是 NUL 结尾的，先拷进 std::string 再交给 cJSON（同 _handleData）。 */
    const std::string json(event->data, static_cast<size_t>(event->data_len));
    cJSON *root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_UTILS_LOGW("命令 JSON 解析失败（%d B）", event->data_len);
        return;
    }

    Command cmd;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "arg");

    if (cJSON_IsString(id) && id->valuestring != nullptr) {
        cmd.id = id->valuestring;
    }
    if (cJSON_IsString(name) && name->valuestring != nullptr) {
        cmd.cmd = name->valuestring;
    }
    if (cJSON_IsString(arg) && arg->valuestring != nullptr) {
        cmd.arg = arg->valuestring;
    }

    cJSON_Delete(root);

    if (cmd.cmd.empty()) {
        ESP_UTILS_LOGW("命令没有 cmd 字段，丢弃（%d B）", event->data_len);
        return;
    }

    ESP_UTILS_LOGI("收到命令 id=%s cmd=%s%s%s", cmd.id.c_str(), cmd.cmd.c_str(),
                   cmd.arg.empty() ? "" : " arg=", cmd.arg.c_str());

    CommandHandler handler;
    {
        std::lock_guard<std::mutex> lock(_cmd_mutex);
        handler = _cmd_handler;
    }
    if (!handler) {
        /* 如实说，别假装收到了 —— 这条日志就是"代码有、但没人接"的唯一证据。 */
        ESP_UTILS_LOGW("没注册命令处理器 ⇒ 只记录不执行");
        return;
    }
    handler(cmd);
}

bool MqttTransport::publishAck(const Command &cmd, bool ok, const char *error, int closed)
{
    if (_client == nullptr || !isConnected()) {
        /* 和 send 同一条契约：断连不排队（恢复后补发一条过期回执只会让人困惑）。 */
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }

    const bool failed = (error != nullptr) && (error[0] != '\0');
    const bool good = ok && !failed;

    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "id", cmd.id.c_str());
    cJSON_AddStringToObject(root, "cmd", cmd.cmd.c_str());
    cJSON_AddBoolToObject(root, "ok", good);
    if (closed >= 0) {
        /* 只有 close_all 会带：让 Pi 能说"已清理 N 个后台"，而不是只说"成功"。 */
        cJSON_AddNumberToObject(root, "closed", closed);
    }
    if (failed) {
        cJSON_AddStringToObject(root, "error", error);
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return false;
    }

    const size_t len = strlen(printed);
    const int msg_id = esp_mqtt_client_publish(_client, TOPIC_ACK, printed, static_cast<int>(len), 1, 0);
    cJSON_free(printed);

    if (msg_id < 0) {
        ESP_UTILS_LOGW("ack 发布失败（%u B）", static_cast<unsigned>(len));
        return false;
    }

    ESP_UTILS_LOGI("已回执 id=%s cmd=%s ok=%d%s%s", cmd.id.c_str(), cmd.cmd.c_str(),
                   good ? 1 : 0, failed ? " error=" : "", failed ? error : "");
    return true;
}

bool MqttTransport::send(const std::string &text, std::string &id)
{
    if (_client == nullptr || !isConnected()) {
        /* 契约：断连期间**不排队**。返回 false，由 UI 告诉用户"未发送（离线）"。 */
        return false;
    }
    if (text.empty()) {
        return false;
    }

    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "p4-%08x-%04u", static_cast<unsigned>(_boot_id), ++_counter);
    id = id_buf;

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(time(nullptr)));
    cJSON_AddStringToObject(root, "text", text.c_str());

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return false;
    }

    const size_t len = strlen(printed);
    const int msg_id = esp_mqtt_client_publish(_client, TOPIC_CHAT, printed, static_cast<int>(len), 1, 0);
    cJSON_free(printed);

    if (msg_id < 0) {
        ESP_UTILS_LOGW("发布失败（%u B）", static_cast<unsigned>(len));
        return false;
    }

    ESP_UTILS_LOGI("已发送 id=%s（%u B）", id.c_str(), static_cast<unsigned>(len));
    return true;
}

} // namespace esp_brookesia::apps::chat
