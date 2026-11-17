/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "brookesia_net.hpp"

#include <utility>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

namespace esp_brookesia::services {
namespace {

constexpr const char *TAG = "NetService";

/* 断线重连节奏：和 `WebConsole::_onStaDisconnected()` 保持一致，
 * 免得同一个事件在两个地方给出两套行为。 */
constexpr int      RETRY_IMMEDIATE  = 5;
constexpr uint32_t RETRY_BACKOFF_US = 15ULL * 1000 * 1000;

/* 「设置 → WLAN」那个开关的状态，`WlanPage` 写的就是这个键（默认 false）。
 * 我们只在**键存在且为 0** 时才认「用户要关网」。 */
constexpr const char *NVS_NAMESPACE   = "storage";
constexpr const char *NVS_KEY_WIFI_EN = "wifi_en";

constexpr EventBits_t BIT_CONNECTED = BIT0;

EventGroupHandle_t        s_events      = nullptr;
esp_timer_handle_t        s_retry_timer = nullptr;
int                       s_retry       = 0;
bool                      s_hooks       = false;
NetService::StateCallback s_state_cb;

void notify(bool connected)
{
    if (s_events != nullptr) {
        if (connected) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);
        } else {
            xEventGroupClearBits(s_events, BIT_CONNECTED);
        }
    }
    if (s_state_cb) {
        s_state_cb(connected);
    }
}

/** 键不存在 / 命名空间不存在 / 读失败 一律按「开」处理 —— 首次配置不该被挡住。 */
bool wifiEnabledByUser()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return true;
    }

    int32_t value = 1;
    const esp_err_t err = nvs_get_i32(handle, NVS_KEY_WIFI_EN, &value);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取 %s/%s 失败(%s)，按「开」处理",
                 NVS_NAMESPACE, NVS_KEY_WIFI_EN, esp_err_to_name(err));
        return true;
    }
    return value != 0;
}

void retryCb(void *)
{
    NetService &self = NetService::getInstance();
    if (!self.isStarted() || self.isConnected()) {
        return;
    }
    esp_wifi_connect();
}

void onGotIp(void *, esp_event_base_t, int32_t, void *data)
{
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    if (event != nullptr) {
        ESP_LOGI(TAG, "WiFi 已连接: " IPSTR, IP2STR(&event->ip_info.ip));
    } else {
        ESP_LOGI(TAG, "WiFi 已连接");
    }
    s_retry = 0;
    notify(true);
}

void onLostIp(void *, esp_event_base_t, int32_t, void *)
{
    ESP_LOGW(TAG, "丢失 IP");
    notify(false);
}

void onDisconnected(void *, esp_event_base_t, int32_t, void *data)
{
    auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
    ESP_LOGW(TAG, "WiFi 断开 (reason=%d)", event != nullptr ? event->reason : -1);
    notify(false);

    NetService &self = NetService::getInstance();
    if (!self.isStarted() || !self.hasCredentials()) {
        return;     /* 没人要连的时候就别插手，免得和设置页抢 */
    }

    ++s_retry;
    if (s_retry <= RETRY_IMMEDIATE) {
        ESP_LOGI(TAG, "立即重连（第 %d 次）", s_retry);
        esp_wifi_connect();
        return;
    }

    /* 连续失败：改成 15 秒一次。**不能在这里 sleep** —— 这是 esp_event 任务，
     * 睡下去会把整个事件循环堵死。 */
    if (s_retry_timer == nullptr || esp_timer_is_active(s_retry_timer)) {
        return;
    }
    ESP_LOGW(TAG, "已连续失败 %d 次，15 秒后再试", s_retry);
    esp_timer_start_once(s_retry_timer, RETRY_BACKOFF_US);
}

} // namespace

NetService &NetService::getInstance()
{
    static NetService instance;
    return instance;
}

bool NetService::start()
{
    if (_started) {
        return true;
    }

    if (!wifiEnabledByUser()) {
        ESP_LOGI(TAG, "「设置 → WLAN」里 WiFi 是关的（NVS %s/%s=0）⇒ 开机不联网",
                 NVS_NAMESPACE, NVS_KEY_WIFI_EN);
        return false;
    }

    /* 出厂 Settings::init() 开机时已经做过 netif/event/wifi_init，
     * 所以下面全部按「幂等兜底」写：做过就跳过，没做过才补。 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
    }

    /* 用 ifkey 判断，别直接调 create —— 它每次都会新建一个 netif，重复调就是泄漏 */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wcfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "WiFi 驱动由 NetService 初始化（本次开机还没人动过）");
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    /* 凭据必须落 NVS —— 出厂设置页存的、`esp_wifi_get_config()` 读的是同一份 */
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    }

    /* 省电模式实测把 ping 往返拉到 64~1035 ms，
     * 而 MQTT 要常驻。开发板 USB 供电，不差这点电 —— 关掉。 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (s_events == nullptr) {
        s_events = xEventGroupCreate();
        if (s_events == nullptr) {
            ESP_LOGE(TAG, "创建事件组失败");
            return false;
        }
    }

    if (s_retry_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &retryCb;
        args.name     = "net_retry";
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) {
            ESP_LOGW(TAG, "创建重连定时器失败：连续失败 5 次后将不再自动重试");
            s_retry_timer = nullptr;
        }
    }

    if (!s_hooks) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onDisconnected, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onGotIp, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &onLostIp, nullptr);
        s_hooks = true;
    }

    _started = true;

    if (!hasCredentials()) {
        ESP_LOGW(TAG, "NVS 里没有 WiFi 凭据 —— 请先去「设置 → WLAN」连一次网");
        return true;
    }

    s_retry = 0;
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "正在连接 NVS 里保存的 WiFi...");
    }

    return true;
}

bool NetService::isConnected() const
{
    if (s_events == nullptr) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool NetService::waitConnected(uint32_t timeout_ms) const
{
    if (s_events == nullptr) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

bool NetService::hasCredentials() const
{
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return false;
    }
    return cfg.sta.ssid[0] != '\0';
}

void NetService::setStateCallback(StateCallback cb)
{
    s_state_cb = std::move(cb);
}

} // namespace esp_brookesia::services
