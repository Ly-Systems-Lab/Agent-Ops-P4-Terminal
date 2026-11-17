/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 网页控制台 app —— 服务本体在 components/brookesia_web_console/，这里只是它的门面：
 * 进页面 → 确保服务起来 → 把「手机该访问哪个地址」用大字显示出来。
 *
 * 屏幕文字一律用英文：板上字体只有拉丁字形（出厂 Brookesia 的 Settings/WLAN 也是英文），
 * 而 LVGL 自带的 CJK 字体是精选字形子集，不保证「网」「页」这些字都有。
 * 中文放在网页里 —— 浏览器字体什么都有。
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps
{

class WebConsoleApp : public systems::phone::App
{
public:
    static WebConsoleApp *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~WebConsoleApp();

protected:
    WebConsoleApp(bool use_status_bar, bool use_navigation_bar);

    /* run() / back() 是纯虚，必须实现 */
    bool run(void) override;
    bool back(void) override;

private:
    static void _onTick(lv_timer_t *timer);
    /** 「Stop Server」：只杀 HTTP 服务器，回桌面后不再占资源；下次进 app 的 run() 会重新拉起。
     * 不动 `/data` 的挂载 —— 见 `WebConsole::stopServer()` 的注释。 */
    static void _onStopClicked(lv_event_t *e);
    /* with_storage = false ⇒ 只刷网络状态（纯 RAM），不碰文件系统。
     * 上传进行中必须传 false：查文件数/容量要去 opendir + 扫 FAT，
     *    会和 httpd 的写盘抢 flash，实测能把上传拖慢约 4 倍。 */
    void _refresh(bool with_storage);

    static WebConsoleApp *_instance;

    /* 下面这些 lv 指针指向默认屏幕上的对象，而框架在退出 app 时会把屏幕销毁
     *    ⇒ 每次 run() 开头**必须全部清空**，否则就是操作已释放对象
     *    （踩过：轻则崩溃重启，重则死锁卡死）。 */
    lv_obj_t   *_root      = nullptr;
    lv_obj_t   *_lbl_ip    = nullptr;
    lv_obj_t   *_lbl_net   = nullptr;
    lv_obj_t   *_lbl_fs    = nullptr;
    lv_obj_t   *_lbl_files = nullptr;
    lv_obj_t   *_lbl_server = nullptr;
    lv_obj_t   *_btn_stop  = nullptr;
    lv_timer_t *_timer     = nullptr;

    /* 刷新节流：空闲时每 2 拍取一次全量；上传期间一拍都不取，结束后立刻补一次 */
    int  _tick      = 0;
    bool _need_full = true;
    bool _prev_busy = false;
};

} // namespace esp_brookesia::apps
