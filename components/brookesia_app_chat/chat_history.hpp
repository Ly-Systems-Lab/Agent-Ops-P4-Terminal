/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 聊天历史落盘 —— `/data/chat/history.jsonl`，一行一条 JSON。
 *
 * 为什么选 /data（16 MB FAT）而不是 spiffs：spiffs 是只读资源分区（固件镜像的一部分），
 * 运行时写不了；/data 是我们在开发早期追加的可写 FAT。
 *
 * 为什么是 JSONL 而不是自造二进制：
 *   ① 字段和 MQTT 报文契约（`docs/mqtt-contract.md`）的报文同名，将来接 MQTT 时能直接对得上；
 *   ② 出问题时人眼能读（`cat` 一下就知道里面是什么）。
 *
 * **写 flash 会卡住整个 SoC 的取指**。这里的写入是**单条追加**
 *    （一条消息几百字节），代价是毫秒级；但如果将来要一次重写整份历史（下面的裁剪），
 *    那是几十 KB 的写，会影响流畅度 —— 所以裁剪阈值放到 400 行才触发一次。
 *
 * 目录不存在时要自己建（/data 上只有网页控制台建过的 `/data/upload`）。
 */
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

namespace esp_brookesia::apps::chat {

enum class Role {
    User,
    Agent,
};

struct Message {
    Role        role = Role::User;
    std::string text;
    bool        error = false;
};

constexpr const char *HISTORY_DIR = "/data/chat";
constexpr const char *HISTORY_PATH = "/data/chat/history.jsonl";

/** 界面上最多展示多少条；超过 TRIM_AT 行时把文件重写成最后 KEEP 条。
 *
 * 50 不是随便挑的：列表里**每条消息都是一个
 *    `LV_LABEL_LONG_WRAP` 的 label**，LVGL 每次布局/绘制都要把这些长文本重新排版
 *    ⇒ 条数越多、单条越长，滑动越像 PPT。200 条长回复实测已经卡到不可用。
 *    这只是**上限**，真正的救命手段是顶栏那个「清空」。
 * 改这里要连着 `HISTORY_TRIM_AT` 一起改（它决定什么时候重写文件）。 */
constexpr int  HISTORY_KEEP = 50;
constexpr int  HISTORY_TRIM_AT = 100;

/** 读文件的上限，防止异常文件把内存吃光。50 条 × 约 400 B ≈ 20 KB。 */
constexpr long HISTORY_MAX_BYTES = 512 * 1024;

namespace detail {

constexpr const char *TAG = "BS:App:Hub:History";

inline bool ensureDir()
{
    struct stat st;
    if (stat(HISTORY_DIR, &st) == 0) {
        return true;
    }
    return mkdir(HISTORY_DIR, 0777) == 0;
}

inline bool parseLine(const char *line, Message &out)
{
    cJSON *root = cJSON_Parse(line);
    if (root == nullptr) {
        return false;
    }

    bool ok = false;
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "r");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(role) && cJSON_IsString(text)) {
        out.role = (role->valuestring[0] == 'a') ? Role::Agent : Role::User;
        out.text = text->valuestring;

        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "e");
        out.error = cJSON_IsNumber(err) && err->valueint != 0;
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

inline std::string toLine(const Message &m)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::string();
    }

    cJSON_AddStringToObject(root, "r", m.role == Role::Agent ? "a" : "u");
    cJSON_AddStringToObject(root, "t", m.text.c_str());
    if (m.error) {
        cJSON_AddNumberToObject(root, "e", 1);
    }

    char *printed = cJSON_PrintUnformatted(root);
    std::string line = printed != nullptr ? printed : "";
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);

    return line;
}

/** 把 `lines` 整体重写回文件（只在裁剪时用）。先写临时文件再 rename。 */
inline bool rewrite(const std::vector<std::string> &lines)
{
    const std::string tmp = std::string(HISTORY_PATH) + ".tmp";

    FILE *fp = fopen(tmp.c_str(), "wb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "无法写 %s", tmp.c_str());
        return false;
    }
    setvbuf(fp, nullptr, _IOFBF, 32 * 1024);

    for (const auto &line : lines) {
        fwrite(line.data(), 1, line.size(), fp);
        fputc('\n', fp);
    }
    fclose(fp);

    remove(HISTORY_PATH);
    if (rename(tmp.c_str(), HISTORY_PATH) != 0) {
        ESP_LOGW(TAG, "rename %s 失败", tmp.c_str());
        return false;
    }
    return true;
}

} // namespace detail

/** 读最近 HISTORY_KEEP 条（按时间正序）。文件不存在 ⇒ 空列表。 */
inline std::vector<Message> historyLoad()
{
    std::vector<Message> messages;

    FILE *fp = fopen(HISTORY_PATH, "rb");
    if (fp == nullptr) {
        return messages;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return messages;
    }
    long size = ftell(fp);
    if (size <= 0 || size > HISTORY_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return messages;
    }

    std::string raw(static_cast<size_t>(size), '\0');
    const size_t got = fread(&raw[0], 1, raw.size(), fp);
    fclose(fp);
    raw.resize(got);

    /* 按行切；顺带记下所有原始行，供下面的裁剪判断用。 */
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) {
            end = raw.size();
        }
        std::string line = raw.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        start = end + 1;
    }

    for (const auto &line : lines) {
        Message m;
        if (detail::parseLine(line.c_str(), m)) {
            messages.push_back(std::move(m));
        }
    }

    /* 裁剪：只在行数明显超标时做一次整体重写（避免每次开 app 都写几十 KB）。 */
    if (static_cast<int>(lines.size()) > HISTORY_TRIM_AT) {
        std::vector<std::string> kept(lines.end() - HISTORY_KEEP, lines.end());
        if (detail::rewrite(kept)) {
            ESP_LOGI(detail::TAG, "历史裁剪：%u 行 → %u 行",
                     static_cast<unsigned>(lines.size()), static_cast<unsigned>(kept.size()));
        }
    }

    if (messages.size() > static_cast<size_t>(HISTORY_KEEP)) {
        messages.erase(messages.begin(), messages.end() - HISTORY_KEEP);
    }

    ESP_LOGI(detail::TAG, "载入 %u 条历史（文件 %ld B）",
             static_cast<unsigned>(messages.size()), size);
    return messages;
}

/** 追加一条。失败只告警，不影响界面（历史丢了不该让聊天用不了）。 */
inline bool historyAppend(const Message &m)
{
    if (!detail::ensureDir()) {
        ESP_LOGW(detail::TAG, "建目录 %s 失败", HISTORY_DIR);
        return false;
    }

    const std::string line = detail::toLine(m);
    if (line.empty()) {
        return false;
    }

    FILE *fp = fopen(HISTORY_PATH, "ab");
    if (fp == nullptr) {
        ESP_LOGW(detail::TAG, "打不开 %s", HISTORY_PATH);
        return false;
    }

    bool ok = fwrite(line.data(), 1, line.size(), fp) == line.size() &&
              fputc('\n', fp) != EOF;
    fclose(fp);

    if (!ok) {
        ESP_LOGW(detail::TAG, "写入失败");
    }
    return ok;
}

/**
 * 清空**本机**历史。
 *
 * 只删我们自己的 `/data/chat/history.jsonl` —— 它记录的是"屏幕上显示过什么"，
 *    **不是**树莓派上 Agent 的上下文。两者毫无关联：Agent 那边的会话状态在 Pi 上，
 *    这里删文件不会让它忘掉任何东西（也不会顺带清掉它）。
 *
 * 判据用 `stat()` 复看，不用 `remove()` 的返回值：FAT 上失败原因很多
 *    （只读挂载、目录项损坏），而**文件还在**才是用户会看到的事实 ——
 *    下次开 app 会把旧记录原样读回来。让日志说实话（判据要能证明真的删掉了）。
 */
inline bool historyClear()
{
    remove(HISTORY_PATH);

    /* `.tmp` 是裁剪时留下的临时文件，一并清掉，免得下次 rewrite 撞上脏数据。 */
    const std::string tmp = std::string(HISTORY_PATH) + ".tmp";
    remove(tmp.c_str());

    struct stat st;
    const bool gone = (stat(HISTORY_PATH, &st) != 0);
    if (gone) {
        ESP_LOGI(detail::TAG, "本机历史已清空（%s）", HISTORY_PATH);
    } else {
        ESP_LOGW(detail::TAG, "删除 %s 失败 —— 下次开 app 还会读到旧记录", HISTORY_PATH);
    }
    return gone;
}

} // namespace esp_brookesia::apps::chat
