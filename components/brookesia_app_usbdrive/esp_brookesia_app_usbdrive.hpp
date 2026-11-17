/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * U 盘模式 app：点进来 = 把板子上的 SD 卡交给 PC 当 U 盘用。
 *
 * 为什么要有这个 app、而不是"插上线就自动切"：
 *   SD 卡在被 PC 独占期间，本地 /sdcard 是**摘掉**的（音乐播放器 / 相册 / 视频播放器
 *   都用不了它）。自动切换会在任意时刻把卡从正在跑的 app 脚下抽走 ⇒ 崩或卡死。
 *   交给用户一个明确的开关，同时也天然提供了"切换前先把访问停下来"的时机。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps
{

class UsbDrive : public systems::phone::App
{
public:
    static UsbDrive *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~UsbDrive();

protected:
    UsbDrive(bool use_status_bar, bool use_navigation_bar);

    // 基类里 run() / back() 是纯虚，必须实现
    bool run(void) override;
    bool back(void) override;

private:
    /** 退出 U 盘模式（幂等）：把 /sdcard 挂回本地。 */
    void _leaveUsbMode(void);

    /**
     * 把屏幕上那些 lv 指针清空 —— **只清不删**（框架自己会删，再删就是二次释放）。
     * 在 run() 开头和 back() 里各调一次，理由与做法同相册。
     */
    void _forgetLvObjects(void);

    static void _onTick(lv_timer_t *t);
    static void _onLeaveClicked(lv_event_t *e);

    /**
     * PC 曾经把我们枚举成功过。
     * 这个成员**跨 run() 存活**（app 是单例），但它是普通 bool，不是 LVGL 对象指针，
     *    所以不受"框架回收 run() 期间建的资源"那条规则影响（那条规则只管 LVGL 对象）。
     *    屏幕上那些 lv_obj_t* 则一律不进成员 —— 每次 run() 重建。
     */
    bool _ever_attached = false;

    /* 屏幕上那两个我们每拍都要改的东西。**不进 run() 之外的逻辑**，
     * 且每次 run() 前先经 _forgetLvObjects() 清空。 */
    lv_obj_t *_status = nullptr;
    lv_timer_t *_tick = nullptr;

    static UsbDrive *_instance;
};

} // namespace esp_brookesia::apps
