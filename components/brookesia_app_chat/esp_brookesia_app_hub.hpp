/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 「Hub」—— P4 上的聊天 / 控制面板。
 *
 * 定位：不是闲聊工具（闲聊用小智），而是**对树莓派上的 Agent 下指令**、
 * 再由它去控制其他端侧设备。所以 UI 目标是"指令看得清、回执看得懂"，
 * 不追求信息密度。
 *
 * 现在这个类**只是视图**：MQTT 客户端、连接状态、回执缓冲都在常驻的
 * `services::HubService` 里 —— 关掉 app 不会断线，app 没打开时收到的回复会
 * 存在服务的未读环形缓冲里，下次进来补写进历史。
 * 所以这里**不建传输层、不启停连接**，只：
 *    ① 注册回执回调（`run()` 里设、退出时清）；
 *    ② 把服务状态画到顶栏；
 *    ③ 发送时调 `HubService::send()`（没连上就是 false，不排队）。
 *
 * 桌面图标上的名字是 "Hub"（ASCII）。不能写中文 —— 启动器那套字体没有中文字形，
 *    会画成乱码。app **内部**的中文没问题，用的是我们自己加载的 20 px 字库。
 */
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "systems/phone/esp_brookesia_phone_app.hpp"

/* `chat::Role` 定义在 chat_history.hpp 里（它属于"消息"而不是"传输"）。 */
#include "chat_history.hpp"
#include "brookesia_hub_service.hpp"

namespace esp_brookesia::apps {

class Hub : public systems::phone::App {
public:
    static Hub *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~Hub();

protected:
    Hub(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    /** id → 那条还在等回执的"…"气泡。文本是逐片累加出来的。 */
    struct Inflight {
        lv_obj_t   *label = nullptr;
        std::string text;
    };

    /* ---- 构建 ---- */
    bool   _ensureFont(void);
    void   _buildUi(lv_obj_t *scr);
    void   _styleCandidatePanel(void);
    /** 把键盘右下角的 "√" 换成"收起键盘"（↓）。 */
    void   _installSwitchKeyMap(lv_obj_t *kb);
    void   _renderHistory(void);
    lv_obj_t *_addBubble(chat::Role role, const std::string &text, bool error);
    /** 列表超过 `HISTORY_KEEP` 行时，从最老的开始删（**运行期**的裁剪）。
     * 载入时的裁剪在 `historyLoad()` 里，两者管的是不同的东西：
     *     那个管"文件里有多少"，这个管"屏幕上挂着多少"。 */
    void _trimBubbles(void);

    /* ---- 交互 ---- */
    void _submit(void);
    void _showKeyboard(bool ascii_mode);
    void _hideKeyboards(void);
    /** 取走服务缓冲的回执快照（全文）—— 可能在 mqtt 任务里被投递，这边只读队列。 */
    void _drainReplies(void);
    /** 取走「app 没开时到达的回复」，补写进历史和界面。 */
    void _drainUnread(void);
    void _applyKeyboardSpace(void);

    /* ---- 清空本机记录：只动我们自己的界面与历史文件，不碰 Agent 上下文 ---- */
    void _clearHistory(void);
    /** 二次确认的 3 秒窗口到点后把按钮复原（由 120 ms 轮询驱动，不另建定时器 —— 框架会回收 run() 期间建的定时器，自己再管一遍就会删两次）。 */
    void _updateClearButton(void);
    static void _onClearClicked(lv_event_t *e);

    /* ---- 连接设置（顶栏入口） ---- */
    void _openConfig(void);
    void _closeConfig(void);
    void _saveConfig(void);
    void _startTest(void);
    void _showConfigMsg(const std::string &text, bool ok);
    void _showConfigKeyboard(lv_obj_t *field);
    void _drainTestResult(void);
    void _updateBadge(void);

    /* ---- 杂项 ---- */
    void _scrollToBottom(void);
    void _dropEmptyHint(void);

    static void _onPoll(lv_timer_t *t);
    static void _onSendClicked(lv_event_t *e);
    static void _onModeClicked(lv_event_t *e);
    static void _onInputClicked(lv_event_t *e);
    static void _onReady(lv_event_t *e);
    static void _onKeyboardValueChanged(lv_event_t *e);
    static void _onConfigClicked(lv_event_t *e);
    static void _onConfigClose(lv_event_t *e);
    static void _onConfigSave(lv_event_t *e);
    static void _onConfigTest(lv_event_t *e);
    static void _onConfigFieldClicked(lv_event_t *e);
    static void _onTlsChanged(lv_event_t *e);

    /**
     * TCP 探活（不是 MQTT 握手）—— 跑在**独立任务**里，结果由 `_onPoll` 取回。
     * 别在 LVGL 任务里做 `getaddrinfo` + `connect`（会卡住整条 UI）。
     */
    static void _testTask(void *arg);

    /**
     * 框架会替我们删掉 `run()` 期间建的 LVGL 对象（`enable_recycle_resource`，
     *    再删一次就是二次释放）。所以这里**只清指针、绝不 del**。
     */
    void _forgetLvObjects(void);

    static Hub *_instance;

    /* ---- LVGL 对象：每次 run() 重建 ---- */
    lv_obj_t   *_root = nullptr;
    lv_obj_t   *_list = nullptr;
    lv_obj_t   *_empty_hint = nullptr;
    lv_obj_t   *_ta = nullptr;
    lv_obj_t   *_kb_ime = nullptr;
    lv_obj_t   *_ime = nullptr;
    lv_obj_t   *_cand_panel = nullptr;  /* 拼音候选栏（由 lv_ime_pinyin 建，我们只改样式与高度） */
    lv_obj_t   *_kb_ascii = nullptr;
    lv_obj_t   *_btn_mode = nullptr;
    lv_timer_t *_poll_timer = nullptr;
    lv_obj_t   *_badge = nullptr;
    lv_obj_t   *_btn_cfg = nullptr;
    lv_obj_t   *_btn_clear = nullptr;
    /** 二次确认的截止时刻（`lv_tick_get()` 毫秒）；0 = 没在等确认。 */
    uint32_t    _clear_armed_until = 0;

    /* ---- 连接设置浮层：只有打开时才存在，其余时刻必须是 nullptr ---- */
    lv_obj_t   *_config_overlay = nullptr;
    lv_obj_t   *_cfg_host = nullptr;
    lv_obj_t   *_cfg_port = nullptr;
    lv_obj_t   *_cfg_user = nullptr;
    lv_obj_t   *_cfg_pass = nullptr;
    lv_obj_t   *_cfg_tls = nullptr;
    lv_obj_t   *_cfg_msg = nullptr;

    /* ---- 常驻：进过一次就一直留着，退出不释放 ---- */
    /* `_font` 是我们加载的 20 px cbin 字库（缺字会回退到 montserrat_20，
     * 因为候选栏翻页用的 LV_SYMBOL_LEFT/RIGHT 它没有）；加载失败时退化成
     * 内置 lv_font_montserrat_20 —— 那时中文是方框，但 app 仍可用。 */
    const lv_font_t *_font = nullptr;
    uint8_t   *_font_data = nullptr;
    bool       _ascii_mode = false;
    int        _kb_height = 0;
    int        _cand_height = 0;

    /* ---- 回执：只做"服务 → LVGL 任务"的搬运，不持有任何连接 ---- */
    std::mutex                         _pending_mutex;
    std::vector<services::ChatReply>   _pending;
    std::map<std::string, Inflight>    _inflight;
    std::string                        _badge_text;

    /* ---- 连接参数 ---- */
    chat::BrokerConfig                 _cfg;

    /* ---- TCP 探活：任务 / LVGL 任务之间的交接（任务那边只写，UI 这边只读+清） ---- */
    std::mutex                         _test_mutex;
    std::string                        _test_host;
    uint16_t                           _test_port = 0;
    bool                               _test_inflight = false;
    bool                               _test_done = false;
    bool                               _test_ok = false;
    std::string                        _test_msg;
};

} // namespace esp_brookesia::apps
