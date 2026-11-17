/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 聊天面板的「传输层接缝」（随 MQTT 一起挪进服务组件）。
 *
 * 目的：把"怎么发出去、回执怎么回来"和 UI 完全隔开。
 *
 * 报文形状严格照 MQTT 报文契约（`docs/mqtt-contract.md`）：
 * `id / seq / delta / final / error` 五个字段。
 *
 * 线程约定：`ReplyHandler` **跑在 esp-mqtt 自己的任务上下文**里。
 *    严禁在 handler 里直接碰 LVGL 对象 —— `HubService` 只把累积好的快照
 *    塞进加锁队列，由 app 的 `lv_timer` 取走。
 *
 * 命名空间仍叫 `esp_brookesia::apps::chat`：这是历史名字。这些
 *    文件从 app 组件挪进服务组件时**故意没改名** —— app 里另一半 `chat::`
 *    （`Role` / `Message` / 历史读写）还留在 app 自己的 `chat_history.hpp` 里，
 *    再引入一个同名的 `esp_brookesia::chat` 会让无限定查找打架。
 *    真要改名，得连历史一起挪进服务那一轮再做。
 */
#pragma once

#include <functional>
#include <string>

namespace esp_brookesia::apps::chat {

/** 一条回执**分片**；字段与 §5.2 的 JSON 一一对应。 */
struct Reply {
    std::string id;      /* 由发起方生成、服务端原样回传 */
    int         seq = 0; /* 同一 id 内的分片序号 */
    std::string delta;   /* 本片新增的文本 */
    bool        final = false;
    std::string error;   /* 非空 ⇒ 这是错误回执，delta 不再有意义 */
};

/**
 * 一条**反控命令**：Pi → P4，主题 `hub/command/p4`。
 *
 * 和 `Reply` 一样是**传输层的裸结构**，不含任何"怎么执行"的知识 ——
 * 执行由 `services::HubCommands` 负责（它需要 LVGL / 框架上下文）。
 *
 * 字段刻意做窄：`id` 只用于 ack 配对（原样回传），`cmd` 是固定的动作名，
 * `arg` 留一个可选的参数位（当前 `music` 用它区分要不要自动播）。
 */
struct Command {
    std::string id;   /* Pi 生成，原样回传；可以为空（那就回一条没有 id 的 ack） */
    std::string cmd;  /* "music" / "aichat" —— 见 `docs/mqtt-contract.md` 第 11 节 */
    std::string arg;  /* 可选："play" 等；空 = 用该命令的默认行为 */
};

class ChatTransport {
public:
    using ReplyHandler = std::function<void(const Reply &)>;

    virtual ~ChatTransport() = default;
    virtual void setReplyHandler(ReplyHandler handler) = 0;

    /** 发一条用户消息。成功则把本次的 id 写进 `id`（回执会带着它回来）。 */
    virtual bool send(const std::string &text, std::string &id) = 0;

    /** 只用于日志/调试显示。 */
    virtual const char *name() const = 0;
};

} // namespace esp_brookesia::apps::chat
