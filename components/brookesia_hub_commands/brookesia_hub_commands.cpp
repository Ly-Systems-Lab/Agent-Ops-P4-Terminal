/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Hub:Commands"
#include "esp_lib_utils.h"

#include <cstdio>
#include <cstring>

#include "MusicPlayer.hpp"
#include "brookesia_hub_commands.hpp"
#include "brookesia_hub_service.hpp"
#include "systems/base/esp_brookesia_base_app.hpp"

namespace esp_brookesia::services {

namespace {

/* 扫安装表找 app 时的上限。id 从 APP_ID_MIN 起连续发放（`_app_free_id++`），
 * 现在的桌面上装了 14 个左右；给到 32 足够，而且**扫到 nullptr 也不会误判**。 */
constexpr int APP_SCAN_MAX = 32;

/* app 名 —— 用的是各自构造函数里的字符串（`MusicPlayer.cpp:163` / `XiaozhiApp.cpp:253`）。
 * 别改成 id：id 是**安装顺序**发的号，加一个 app 就会全体错位。 */
constexpr const char *APP_NAME_MUSIC = "MusicPlayer";
constexpr const char *APP_NAME_AICHAT = "AIChats";

} // namespace

HubCommands &HubCommands::getInstance()
{
    static HubCommands instance;
    return instance;
}

bool HubCommands::begin(systems::base::Context *context)
{
    if (_context != nullptr) {
        return true;   /* 幂等 */
    }
    if (context == nullptr) {
        ESP_UTILS_LOGE("begin() 拿到空的 Context ⇒ 反控不可用");
        return false;
    }

    _context = context;

    if (_queue == nullptr) {
        _queue = xQueueCreate(QUEUE_LEN, sizeof(Pending));
        if (_queue == nullptr) {
            ESP_UTILS_LOGE("命令队列创建失败（内部 RAM 不够？）");
            _context = nullptr;
            return false;
        }
    }

    /* **常驻**注册：Hub app 开不开都有效（这正是它跟回执 handler 的区别）。 */
    HubService::getInstance().setCommandHandler(&HubCommands::_onMqttCommand);

    /* 这里必须已经在 LVGL 锁里（main.cpp 的 LvLockGuard 块）——
     * `lv_timer_create` 是往 LVGL 的全局定时器链表插一项（LVGL 任务随时会遍历同一条链表，增删必须持锁）。 */
    if (_timer == nullptr) {
        _timer = lv_timer_create(&HubCommands::_onPoll, POLL_PERIOD_MS, nullptr);
        if (_timer == nullptr) {
            ESP_UTILS_LOGE("lv_timer 创建失败 ⇒ 命令能收但不会执行");
            _context = nullptr;
            return false;
        }
    }

    ESP_UTILS_LOGI("反控就绪：hub/command/p4 → music / aichat / close_all（轮询 %u ms，队列 %u）",
                   static_cast<unsigned>(POLL_PERIOD_MS), static_cast<unsigned>(QUEUE_LEN));
    return true;
}

/* ==========================================================================
 * esp-mqtt 任务：只入队
 * ========================================================================== */

void HubCommands::_onMqttCommand(const chat::Command &cmd)
{
    HubCommands &self = getInstance();

    if (self._queue == nullptr) {
        ESP_UTILS_LOGW("执行器还没 begin() ⇒ 命令丢弃（cmd=%s）", cmd.cmd.c_str());
        return;
    }

    Pending pending = {};
    std::snprintf(pending.id, sizeof(pending.id), "%s", cmd.id.c_str());
    std::snprintf(pending.cmd, sizeof(pending.cmd), "%s", cmd.cmd.c_str());
    std::snprintf(pending.arg, sizeof(pending.arg), "%s", cmd.arg.c_str());

    /* 截断必须是**响的**：id 对不上，Pi 那边就只能看到一条没有配对的 ack。 */
    if (cmd.id.size() >= sizeof(pending.id) || cmd.cmd.size() >= sizeof(pending.cmd) ||
            cmd.arg.size() >= sizeof(pending.arg)) {
        ESP_UTILS_LOGW("命令字段超长被截断：id=%u/%u cmd=%u/%u arg=%u/%u",
                       static_cast<unsigned>(cmd.id.size()), static_cast<unsigned>(sizeof(pending.id)),
                       static_cast<unsigned>(cmd.cmd.size()), static_cast<unsigned>(sizeof(pending.cmd)),
                       static_cast<unsigned>(cmd.arg.size()), static_cast<unsigned>(sizeof(pending.arg)));
    }

    if (xQueueSend(self._queue, &pending, 0) != pdTRUE) {
        ESP_UTILS_LOGW("命令队列满（%u），丢弃 cmd=%s", static_cast<unsigned>(QUEUE_LEN), pending.cmd);
        self._ack(pending, false, "busy");
        return;
    }

    ESP_UTILS_LOGI("命令入队 cmd=%s arg=%s（等 LVGL 任务执行）",
                   pending.cmd, pending.arg[0] != '\0' ? pending.arg : "-");
}

/* ==========================================================================
 * LVGL 任务：出队 → 执行 → 回 ack
 * ========================================================================== */

void HubCommands::_onPoll(lv_timer_t *)
{
    HubCommands &self = getInstance();
    if (self._queue == nullptr) {
        return;
    }

    Pending pending;
    while (xQueueReceive(self._queue, &pending, 0) == pdTRUE) {
        self._execute(pending);
    }
}

void HubCommands::_execute(const Pending &pending)
{
    /* `close_all` 是**全局**命令：没有目标 app，也不经过 `_findAppId`。
 * 先处理它，免得被下面"未知命令"的分支拦掉。 */
    if (std::strcmp(pending.cmd, "close_all") == 0) {
        _closeAll(pending);
        return;
    }

    const char *app_name = nullptr;
    bool close = false;
    bool autoplay = false;

    if (std::strcmp(pending.cmd, "music") == 0) {
        app_name = APP_NAME_MUSIC;
    } else if (std::strcmp(pending.cmd, "aichat") == 0) {
        app_name = APP_NAME_AICHAT;
    } else {
        ESP_UTILS_LOGW("未知命令 '%s'（当前只有 music / aichat）", pending.cmd);
        _ack(pending, false, "unknown_cmd");
        return;
    }

    /* `arg` 的正规取值（两条命令共用同一套）：
     *   空 / "play"  = 打开并开始（只有 music 有"开始"这个动作）
     *   "open"       = 只打开，不出声（music 的备用手刹）
     *   "close"/"stop" = **关掉 app 回桌面**
     * 别的值一律报 bad_arg —— 别让打错字变成"点了没反应"。 */
    if (std::strcmp(pending.arg, "close") == 0 || std::strcmp(pending.arg, "stop") == 0) {
        close = true;
    } else if (pending.arg[0] == '\0' || std::strcmp(pending.arg, "play") == 0) {
        autoplay = (std::strcmp(pending.cmd, "music") == 0);
    } else if (std::strcmp(pending.arg, "open") == 0) {
        autoplay = false;
    } else {
        ESP_UTILS_LOGW("%s 的 arg 不认识：'%s'（只支持 play / open / close）",
                       pending.cmd, pending.arg);
        _ack(pending, false, "bad_arg");
        return;
    }

    const int app_id = _findAppId(app_name);
    if (app_id < 0) {
        ESP_UTILS_LOGE("桌面上找不到 app '%s' ⇒ 这条命令没法执行（是不是改名了？）", app_name);
        _ack(pending, false, "not_installed");
        return;
    }

    if (close) {
        /* ================= 反向：关掉 app =================
         * 这条路和"最近任务里点垃圾桶"**完全等价** —— 框架自己在那个按钮里发的
         * 就是 `AppEventType::STOP`（`esp_brookesia_phone_manager.cpp:1207-1222`）。
         * `App::processClose()` 会**无条件**先调 `close()`（base_app.cpp:537），
         * 所以 MusicPlayer 即使在后台放着，`audio_player_pause()` + `bsp_extra_player_del()`
         * 也一定会把声音停掉 —— 这正是用户要的"释放后台"。 */
        systems::base::Manager &manager = _context->getManager();

        /* 幂等：本来就没在跑，视作"已经是关闭状态"，别报错。 */
        if (manager.getRunningAppById(app_id) == nullptr) {
            ESP_UTILS_LOGI("app '%s' 本来就没在运行 ⇒ 视作已关闭", app_name);
            _ack(pending, true, nullptr);
            return;
        }

        systems::base::Context::AppEventData event = {};
        event.id = app_id;
        event.type = systems::base::Context::AppEventType::STOP;
        event.data = nullptr;
        if (!_context->sendAppEvent(&event)) {
            ESP_UTILS_LOGE("向框架发 STOP 事件失败（app=%s id=%d）", app_name, app_id);
            _ack(pending, false, "event_failed");
            return;
        }

        /* 判据同上：不看返回值，看它**还在不在**运行表里（中间层的返回值只说明它那一步）。 */
        if (manager.getRunningAppById(app_id) != nullptr) {
            ESP_UTILS_LOGE("STOP 事件发出去了，但 app '%s' 还在运行表里 ⇒ 关闭失败", app_name);
            _ack(pending, false, "close_failed");
            return;
        }

        ESP_UTILS_LOGI("已关闭 app '%s'（id=%d）", app_name, app_id);
        _ack(pending, true, nullptr);
        return;
    }

    /* ================= 正向：打开 =================
     * `sendAppEvent` 是**同步**的：里面有 `lv_obj_send_event` → 框架的 `startApp()`
     * → 我们的 `run()`，返回时屏幕已经换好了（它会写 LVGL 的全局事件链表，所以只能在 LVGL 任务里调）。 */
    systems::base::Context::AppEventData event = {};
    event.id = app_id;
    event.type = systems::base::Context::AppEventType::START;
    event.data = nullptr;
    if (!_context->sendAppEvent(&event)) {
        ESP_UTILS_LOGE("向框架发 START 事件失败（app=%s id=%d）", app_name, app_id);
        _ack(pending, false, "event_failed");
        return;
    }

    /* 别把 `sendAppEvent()` 的返回值当"真的跑起来了"：它只是 `lv_obj_send_event`
     * 的结果，而框架里失败分支是 `log + return`（`ESP_UTILS_CHECK_FALSE_EXIT`）。
     * 真正的判据是"它现在在运行表里" —— 让 ack 说实话。 */
    if (_context->getManager().getRunningAppById(app_id) == nullptr) {
        ESP_UTILS_LOGE("START 事件发出去了，但 app '%s' 没进运行表 ⇒ 启动失败", app_name);
        _ack(pending, false, "start_failed");
        return;
    }

    if (autoplay) {
        /* 和 UI 是**同一个 LVGL 任务**，所以 `run()` 已经把 `lv_demo_music()` 建好了，
         * 这里直接点第 1 首（等价于用户点了列表的第一行）。 */
        apps::MusicPlayer *music = apps::MusicPlayer::requestInstance();
        if (music == nullptr || !music->playFirst()) {
            /* 界面开了但没响：如实回 failed，别让 Pi 以为"已经放上了"。 */
            ESP_UTILS_LOGW("MusicPlayer 已打开，但第 1 首播不起来（列表为空 / 播放器没就绪）");
            _ack(pending, false, "no_track");
            return;
        }
        ESP_UTILS_LOGI("MusicPlayer 已打开并开始播第 1 首");
    } else {
        ESP_UTILS_LOGI("已打开 app '%s'（id=%d）", app_name, app_id);
    }

    _ack(pending, true, nullptr);
}

/* ==========================================================================
 * close_all：一键清理所有后台（等价于最近任务里的垃圾桶）
 * ========================================================================== */

void HubCommands::_closeAll(const Pending &pending)
{
    systems::base::Manager &manager = _context->getManager();

    /* **先把 id 抄出来，再逐个关**：关闭会改运行表，
     * 边遍历 `getRunningAppByIdenx()` 边删是未定义行为。
     * 16 个槽远超 `max_running_num`（本机样式表是 3），多出来的只告警不静默丢。 */
    int ids[16] = {};
    int count = 0;

    const uint8_t running = manager.getRunningAppCount();
    for (uint8_t i = 0; i < running; ++i) {
        systems::base::App *app = manager.getRunningAppByIdenx(i);
        if (app == nullptr) {
            continue;
        }
        if (count >= static_cast<int>(sizeof(ids) / sizeof(ids[0]))) {
            ESP_UTILS_LOGW("close_all：运行中的 app 超过 %d 个，多余的没关（上限写小了？）",
                           static_cast<int>(sizeof(ids) / sizeof(ids[0])));
            break;
        }
        ids[count++] = app->getId();
    }

    int closed = 0;
    for (int i = 0; i < count; ++i) {
        /* 前一次关闭可能顺带把它也带走了（比如关掉的正是唯一前台 app），跳过。 */
        if (manager.getRunningAppById(ids[i]) == nullptr) {
            continue;
        }

        systems::base::Context::AppEventData event = {};
        event.id = ids[i];
        event.type = systems::base::Context::AppEventType::STOP;
        event.data = nullptr;
        if (!_context->sendAppEvent(&event)) {
            ESP_UTILS_LOGW("close_all：给 app id=%d 发 STOP 失败，继续下一个", ids[i]);
            continue;
        }
        if (manager.getRunningAppById(ids[i]) == nullptr) {
            ++closed;
        } else {
            ESP_UTILS_LOGW("close_all：app id=%d 发完 STOP 还在运行表里", ids[i]);
        }
    }

    const uint8_t left = manager.getRunningAppCount();
    ESP_UTILS_LOGI("close_all：运行中 %u 个 → 关掉 %d 个 → 剩 %u 个",
                   static_cast<unsigned>(running), closed, static_cast<unsigned>(left));

    if (left != 0) {
        /* 有没关掉的就说实话，别回 ok 让 Pi 以为清干净了。 */
        _ack(pending, false, "close_failed", closed);
        return;
    }
    _ack(pending, true, nullptr, closed);
}

int HubCommands::_findAppId(const char *name) const
{
    if (_context == nullptr || name == nullptr) {
        return -1;
    }

    systems::base::Manager &manager = _context->getManager();
    for (int id = systems::base::App::APP_ID_MIN;
            id < systems::base::App::APP_ID_MIN + APP_SCAN_MAX; ++id) {
        systems::base::App *app = manager.getInstalledApp(id);
        if (app == nullptr) {
            continue;
        }
        const char *app_name = app->getName();
        if (app_name != nullptr && std::strcmp(app_name, name) == 0) {
            return id;
        }
    }
    return -1;
}

void HubCommands::_ack(const Pending &pending, bool ok, const char *error, int closed)
{
    chat::Command cmd;
    cmd.id = pending.id;
    cmd.cmd = pending.cmd;
    /* 没连上时 `publishAck` 返回 false —— 那条路只打一行 warn，不再重试（不排队）。 */
    HubService::getInstance().publishAck(cmd, ok, error, closed);
}

} // namespace esp_brookesia::services