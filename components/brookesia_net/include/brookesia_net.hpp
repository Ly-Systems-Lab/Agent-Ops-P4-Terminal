/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 开机自动联网 —— 常驻的 WiFi 服务，**不是 app**。
 *
 * 为什么需要它：出厂固件只在两个地方 `esp_wifi_start()` ——
 *   ① 设置 → WLAN 页（`WlanPage::run()`），
 *   ② 网页控制台 app（`WebConsole::_ensureWifi()`）。
 * 也就是说**没人点过这两处，开机就是没网的**。而聊天/控制面板要常驻 MQTT，
 * 网络必须开机就有。所以把「起 WiFi + 用 NVS 里已有的凭据连上 + 断了自动重连」
 * 独立成这个服务。
 *
 * 边界（用户明确要求）：
 *   - **不碰网页控制台**。那个服务必须手动点 app 才起，不许自启动；
 *     这里的 `start()` 只动 WiFi，不建 httpd、不挂 /data。
 *   - 断线重连策略与 `WebConsole::_onStaDisconnected()` 一致
 *     （立即重试 5 次，之后 15 秒一次）。控制台开着时两边各连一次是无害的：
 *     `esp_wifi_connect()` 重复调用只返回 `ESP_ERR_WIFI_CONN`。
 *
 * 与「设置 → WLAN」那个开关的关系（2026-10-23 用户定）：
 *   尊重 NVS `storage/wifi_en`。用户主动关过 WiFi ⇒ 开机不连、连射频都不起；
 *   键不存在（从没进过 WLAN 页）⇒ 按「开」处理，避免首次配置被挡住。
 */
#pragma once

#include <cstdint>
#include <functional>

namespace esp_brookesia::services {

class NetService {
public:
    /** 连接状态变化回调。**跑在 esp_event 任务上下文里**，别在里面做重活/阻塞。 */
    using StateCallback = std::function<void(bool connected)>;

    static NetService &getInstance();

    NetService(const NetService &) = delete;
    NetService &operator=(const NetService &) = delete;

    /**
     * 起 WiFi 并尝试连接。**幂等**：重复调用直接返回当前状态。
     *
     * @return true  = WiFi 已经起来（连不连得上是异步的，用 isConnected()/waitConnected() 看）
     *         false = 没起来（用户关了 wifi_en，或初始化失败）
     *
     * 会阻塞一小段时间（`esp_wifi_init`/`esp_wifi_start` 要等 esp-hosted 那边就绪），
     *    所以调用点放在开机流程**最后**（桌面已经出来了）。
     */
    bool start();

    /** WiFi 射频是否已由本服务启动。 */
    bool isStarted() const
    {
        return _started;
    }

    /** 是否已经拿到 IP。 */
    bool isConnected() const;

    /** 等到拿到 IP 为止；超时返回 false。给后续 MQTT 服务用。 */
    bool waitConnected(uint32_t timeout_ms) const;

    /** NVS 里是否有可用凭据（SSID 非空）。 */
    bool hasCredentials() const;

    /** 设置连接状态回调（单槽，后设的覆盖前面的）。 */
    void setStateCallback(StateCallback cb);

private:
    NetService() = default;
    ~NetService() = default;

    bool _started = false;
};

} // namespace esp_brookesia::services
