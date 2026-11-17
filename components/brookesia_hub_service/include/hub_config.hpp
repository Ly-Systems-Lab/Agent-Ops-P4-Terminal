/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hub 的「树莓派连接参数」—— NVS 持久化 + 校验。
 *
 * 为什么现在就要做：Pi 侧的 broker **强制** `password_file` + ACL（见
 * `docs/mqtt-contract.md` 第 2 节），所以光有 IP 是连不上的 ——
 * 主机 / 端口 / 用户名 / 密码 / 是否加密 这五项必须一起可配。
 *
 * NVS 命名空间用 `hub`（与主题前缀、app 名一致）。
 * **密码在 NVS 里是明文**（本板未启用 flash 加密）。这是"局域网家用设备"的取舍，
 *   刻意不做二次加密：密钥同样得存在设备上，只会变成自欺欺人。**任何日志都不许打印它。**
 *
 * 调用前必须已经 `nvs_flash_init()` 成功（`main.cpp` 早就做了）。
 */
#pragma once

#include <cstdint>
#include <string>

#include "nvs.h"
#include "nvs_flash.h"

namespace esp_brookesia::apps::chat {

constexpr const char *CONFIG_NVS_NAMESPACE = "hub";
constexpr const char *CFG_KEY_HOST = "host";
constexpr const char *CFG_KEY_PORT = "port";
constexpr const char *CFG_KEY_USER = "user";
constexpr const char *CFG_KEY_PASS = "pass";
constexpr const char *CFG_KEY_TLS  = "tls";

/* 两个默认端口跟着加密开关走（8883 是 MQTT over TLS 的约定端口）。 */
constexpr uint16_t BROKER_PORT_PLAIN = 1883;
constexpr uint16_t BROKER_PORT_TLS = 8883;

/** NVS 单键能存的长度上限（ESP-IDF 对 str 类型约 4000 B，这里取个够用的宽松值）。 */
constexpr size_t BROKER_FIELD_MAX = 63;

struct BrokerConfig {
    std::string host;                                   /* IP 或主机名，不含端口 */
    uint16_t    port = BROKER_PORT_PLAIN;
    std::string user;
    std::string pass;
    bool        tls = false;

    bool isConfigured() const
    {
        return !host.empty() && port != 0;
    }

    std::string summary() const
    {
        return host + ":" + std::to_string(port);
    }
};

namespace internal {

inline std::string trim(const std::string &s)
{
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ||
                           s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/** 读一个字符串键。不存在 / 太长 / 读失败都当作"没设"。 */
inline bool readStr(nvs_handle_t handle, const char *key, std::string &out)
{
    size_t len = 0;
    if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len == 0 ||
            len > BROKER_FIELD_MAX + 1) {
        return false;
    }

    char buf[BROKER_FIELD_MAX + 2];
    if (nvs_get_str(handle, key, buf, &len) != ESP_OK) {
        return false;
    }
    out = buf;
    return true;
}

} // namespace internal

/** 读当前配置。没存过 ⇒ host 为空（`isConfigured()` 为 false）。 */
inline BrokerConfig configLoad(void)
{
    BrokerConfig cfg;

    nvs_handle_t handle;
    if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return cfg;
    }

    internal::readStr(handle, CFG_KEY_HOST, cfg.host);
    internal::readStr(handle, CFG_KEY_USER, cfg.user);
    internal::readStr(handle, CFG_KEY_PASS, cfg.pass);

    int32_t port = 0;
    if (nvs_get_i32(handle, CFG_KEY_PORT, &port) == ESP_OK && port > 0 && port <= 65535) {
        cfg.port = static_cast<uint16_t>(port);
    }

    uint8_t tls = 0;
    if (nvs_get_u8(handle, CFG_KEY_TLS, &tls) == ESP_OK) {
        cfg.tls = (tls != 0);
    }

    nvs_close(handle);

    /* 从来没存过端口 ⇒ 按加密开关给一个合理默认，而不是留 0。 */
    if (cfg.port == 0) {
        cfg.port = cfg.tls ? BROKER_PORT_TLS : BROKER_PORT_PLAIN;
    }
    return cfg;
}

/** 整体写入并提交。任一步失败 ⇒ 返回 false（调用方负责提示用户）。 */
inline bool configSave(const BrokerConfig &cfg)
{
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_str(handle, CFG_KEY_HOST, cfg.host.c_str());
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, CFG_KEY_PORT, static_cast<int32_t>(cfg.port));
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, CFG_KEY_USER, cfg.user.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, CFG_KEY_PASS, cfg.pass.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CFG_KEY_TLS, cfg.tls ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err == ESP_OK;
}

/** 校验并返回人话错误（空串 = 通过）。UI 直接把它显示给用户。 */
inline std::string configValidate(const BrokerConfig &cfg)
{
    if (cfg.host.empty()) {
        return "请填写树莓派的 IP";
    }
    if (cfg.host.size() > BROKER_FIELD_MAX) {
        return "IP / 主机名太长";
    }
    for (const char ch : cfg.host) {
        if (static_cast<unsigned char>(ch) <= ' ' || ch == '/' || ch == ':') {
            return "IP 里不能有空格或 : / —— 端口填在下面一格";
        }
    }
    if (cfg.port == 0) {
        return "端口要在 1~65535 之间";
    }
    if (cfg.user.size() > BROKER_FIELD_MAX) {
        return "用户名太长";
    }
    if (cfg.pass.size() > BROKER_FIELD_MAX) {
        return "密码太长";
    }
    return std::string();
}

} // namespace esp_brookesia::apps::chat
