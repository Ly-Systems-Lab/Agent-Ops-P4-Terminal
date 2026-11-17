/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:App:WebConsole"
#include "esp_lib_utils.h"

#include "esp_brookesia_app_webconsole.hpp"

#include "esp_brookesia_web_console.hpp"

using namespace esp_brookesia::systems;

#define APP_NAME "Web Console"

/* 桌面图标（112x112 ARGB8888），见 assets/esp_brookesia_image_large_app_webconsole_112_112.c */
extern const lv_image_dsc_t esp_brookesia_image_large_app_webconsole_112_112;

namespace esp_brookesia::apps
{

WebConsoleApp *WebConsoleApp::_instance = nullptr;

WebConsoleApp *WebConsoleApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new WebConsoleApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

/* use_default_screen = true ⇒ 我们直接画在 lv_scr_act() 上，退出时框架自动回收。 */
WebConsoleApp::WebConsoleApp(bool use_status_bar, bool use_navigation_bar) :
    App(APP_NAME, &esp_brookesia_image_large_app_webconsole_112_112, true, use_status_bar, use_navigation_bar)
{
}

WebConsoleApp::~WebConsoleApp()
{
    ESP_UTILS_LOGD("Destroy(@0x%p)", this);
}

namespace
{

/* 一行「标题 —— 值」，值是我们要随时刷新的标签 */
lv_obj_t *makeRow(lv_obj_t *parent, const char *caption, lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = lv_label_create(row);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x8B95A3), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_16, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "—");
    lv_obj_set_style_text_color(val, lv_color_hex(0xE6E9EE), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);

    if (value_out != nullptr) {
        *value_out = val;
    }
    return row;
}

} // namespace

bool WebConsoleApp::run(void)
{
    ESP_UTILS_LOGD("Run(@0x%p)", this);

    /* 必须先清空：上一次退出时框架已经把屏幕连上面的对象一起销毁了 */
    _root      = nullptr;
    _lbl_ip    = nullptr;
    _lbl_net   = nullptr;
    _lbl_fs    = nullptr;
    _lbl_files = nullptr;
    _lbl_server = nullptr;
    _btn_stop  = nullptr;
    _timer     = nullptr;
    _tick      = 0;
    _need_full = true;
    _prev_busy = false;

    /* 起服务（幂等）：挂数据分区 → 确保 WiFi 起来 → 开 HTTP 服务器。
     * 首次上电会把 16 MB 数据分区格成 FAT，这里会阻塞一下下 —— 日志里有耗时。 */
    const bool ok = services::WebConsole::getInstance().start();
    if (!ok) {
        ESP_UTILS_LOGE("网页控制台启动失败（数据分区挂不上？）");
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101417), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    _root = scr;

    /* getVisualArea() 是框架给的「不被状态栏/导航栏挡住」的区域，
     * 宽高直接由它算出来 —— 不要去 set_size 之后再 get_width()，
     * 那样读到的是 0（LVGL 9 的 set_size 是延迟生效的）。 */
    const lv_area_t &area = getVisualArea();
    const int vw = static_cast<int>(lv_area_get_width(&area));
    const int vh = static_cast<int>(lv_area_get_height(&area));

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, vw, vh);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_row(box, 16, 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ok ? "Web Console" : "Web Console (start failed)");
    lv_obj_set_style_text_color(title, lv_color_hex(0x5AC8FA), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    /* 访问地址：整个页面最重要的一行 */
    _lbl_ip = lv_label_create(box);
    lv_label_set_text(_lbl_ip, "…");
    lv_obj_set_style_text_font(_lbl_ip, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_lbl_ip, lv_color_hex(0x4CD964), 0);
    lv_label_set_long_mode(_lbl_ip, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_lbl_ip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_lbl_ip, LV_PCT(100));

    lv_obj_t *hint = lv_label_create(box);
    lv_label_set_text(hint, "Open this address in your phone browser");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8B95A3), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, LV_PCT(100));

    makeRow(box, "Wi-Fi",   &_lbl_net);
    makeRow(box, "Storage", &_lbl_fs);
    makeRow(box, "Files",   &_lbl_files);
    makeRow(box, "Server",  &_lbl_server);

    /* 「Stop Server」—— 把后台 HTTP 服务杀掉。
     * 定位：这个 app 平时是当**文件管理入口**用的（删文件、看目录），
     * 用完就关掉，不必让它常驻吃任务栈和 socket。
     * 屏幕文字一律 ASCII（本 app 只用 montserrat，它没有中文字形）。 */
    _btn_stop = lv_button_create(box);
    lv_obj_set_size(_btn_stop, 260, 52);
    lv_obj_set_style_radius(_btn_stop, 14, 0);
    lv_obj_set_style_bg_color(_btn_stop, lv_color_hex(0x8C2F28), 0);
    lv_obj_add_event_cb(_btn_stop, &WebConsoleApp::_onStopClicked, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(_btn_stop);
        lv_label_set_text(label, "Stop Server");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }

    _refresh(true);

    /* 每秒刷一次状态（连上网/收到文件后屏幕会自己变） */
    _timer = lv_timer_create(&WebConsoleApp::_onTick, 1000, this);

    return true;
}

void WebConsoleApp::_onTick(lv_timer_t *timer)
{
    auto *self = static_cast<WebConsoleApp *>(timer->user_data);
    if (self == nullptr) {
        return;
    }

    const bool busy = services::WebConsole::getInstance().isStorageBusy();

    /* 存储操作刚结束 ⇒ 下一拍立刻补一次全量，把文件数/容量刷成最新 */
    if (self->_prev_busy && !busy) {
        self->_need_full = true;
    }
    self->_prev_busy = busy;

    /* 上传进行中**绝不**查文件系统 —— 那是 opendir + 扫 FAT，会和 httpd 的写盘抢 flash。
     * 实测：这条噪声把 512 KB 上传从 ~120 KB/s 拖到 ~35 KB/s（约 4 倍）。 */
    const bool full = !busy && (((self->_tick % 2) == 0) || self->_need_full);
    if (full) {
        self->_need_full = false;
    }
    self->_tick++;

    self->_refresh(full);
}

void WebConsoleApp::_refresh(bool with_storage)
{
    /* 屏幕被销毁后定时器可能还跑一拍，这里兜住 */
    if (_root == nullptr || !lv_obj_is_valid(_root)) {
        return;
    }

    const services::WebConsole::Status st =
        services::WebConsole::getInstance().getStatus(with_storage);

    if (_lbl_ip != nullptr) {
        const bool has_ip = st.wifi_connected && st.ip[0] != '\0';
        lv_label_set_text(_lbl_ip, has_ip ? st.ip : "no network");
        lv_obj_set_style_text_color(_lbl_ip,
                                    lv_color_hex(has_ip ? 0x4CD964 : 0xFF6B6B), 0);
    }
    if (_lbl_net != nullptr) {
        lv_label_set_text(_lbl_net, st.wifi_connected ? "connected" : "disconnected");
    }

    /* 服务器状态必须在**早退之前**刷：`with_storage=false` 也会走到这里。 */
    if (_lbl_server != nullptr) {
        lv_label_set_text(_lbl_server, st.running ? "running" : "stopped");
        lv_obj_set_style_text_color(_lbl_server,
                                    lv_color_hex(st.running ? 0x4CD964 : 0xFF6B6B), 0);
    }
    if (_btn_stop != nullptr) {
        /* 停下来之后按钮就没有下一步了（不提供"再启动"—— 用户口径是"下次进 app 再说"），
         * 直接置灰并改字，免得用户反复点一个不会有效果的按钮。 */
        lv_obj_t *label = lv_obj_get_child(_btn_stop, 0);
        if (st.running) {
            lv_obj_remove_state(_btn_stop, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(_btn_stop, lv_color_hex(0x8C2F28), 0);
            if (label != nullptr) {
                lv_label_set_text(label, "Stop Server");
            }
        } else {
            lv_obj_add_state(_btn_stop, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(_btn_stop, lv_color_hex(0x2A3238), 0);
            if (label != nullptr) {
                lv_label_set_text(label, "Server stopped");
            }
        }
    }

    if (!with_storage) {
        return;     /* 这两项没查，保留上一次的值 */
    }

    constexpr unsigned long long MB = 1024ull * 1024ull;

    if (_lbl_fs != nullptr) {
        /* 不用 %f —— lv_label_set_text_fmt 走 LVGL 自己的 printf，浮点要 LV_USE_FLOAT 才支持。
         * 整数拼一位小数。**两项都要带小数**：否则 "15.8 / 15 MB" 看着像可用比总量还大。 */
        lv_label_set_text_fmt(_lbl_fs, "%u.%u / %u.%u MB free",
                              static_cast<unsigned>(st.fs_free / MB),
                              static_cast<unsigned>((st.fs_free % MB) * 10ull / MB),
                              static_cast<unsigned>(st.fs_total / MB),
                              static_cast<unsigned>((st.fs_total % MB) * 10ull / MB));
    }
    if (_lbl_files != nullptr) {
        lv_label_set_text_fmt(_lbl_files, "%d", st.file_count);
    }
}

void WebConsoleApp::_onStopClicked(lv_event_t *e)
{
    auto *self = static_cast<WebConsoleApp *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }

    services::WebConsole::getInstance().stopServer();
    ESP_UTILS_LOGI("用户点了 Stop Server：HTTP 服务已停，/data 保持挂载");

    /* 只刷网络/服务器状态，不碰文件系统（上传刚结束的话也没必要再扫一遍 FAT）。 */
    self->_refresh(false);
}

bool WebConsoleApp::back(void)
{
    ESP_UTILS_LOGD("Back(@0x%p)", this);

    /* 先撤定时器 —— 框架紧接着就会销毁屏幕，定时器再跑一拍就是操作已释放对象 */
    if (_timer != nullptr) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
    _root      = nullptr;
    _lbl_ip    = nullptr;
    _lbl_net   = nullptr;
    _lbl_fs    = nullptr;
    _lbl_files = nullptr;
    _lbl_server = nullptr;
    _btn_stop  = nullptr;

    /* 注意：**这里故意不停 HTTP 服务器**。
     * 退出这个页面回到桌面之后，手机照样能上传文件 —— 服务器跟单例走，不跟屏幕走。 */

    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");

    return true;
}

} // namespace esp_brookesia::apps
