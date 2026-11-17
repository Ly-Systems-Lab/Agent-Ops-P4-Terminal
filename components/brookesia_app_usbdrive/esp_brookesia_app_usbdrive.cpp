/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:App:UsbDrive"
#include "esp_lib_utils.h"

#include "brookesia_usb_storage.hpp"

#include "esp_brookesia_app_usbdrive.hpp"

using namespace esp_brookesia::systems;
using esp_brookesia::services::UsbStorage;

#define APP_NAME            "USB Drive"

/* 桌面图标：tb02.webp 转成的 LVGL ARGB8888 资源，112x112，
 * 与原厂 app 图标同规格。资源在 assets/img_app_usbdrive.c。 */
LV_IMG_DECLARE(img_app_usbdrive);

/* 屏幕上出现的一切文字都必须是 ASCII —— LVGL 内置的 montserrat 字体一个中文字形都没有，
 * 写成中文是**静默**乱码（不报错、不影响别的）。 */
#define TXT_TITLE           "USB DRIVE MODE"
#define TXT_INIT            "Preparing..."

namespace esp_brookesia::apps {

UsbDrive *UsbDrive::_instance = nullptr;

UsbDrive *UsbDrive::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new UsbDrive(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

UsbDrive::UsbDrive(bool use_status_bar, bool use_navigation_bar) :
    App(APP_NAME, &img_app_usbdrive, true, use_status_bar, use_navigation_bar)
{
}

UsbDrive::~UsbDrive()
{
    ESP_UTILS_LOGD("Destroy(@0x%p)", this);
}

bool UsbDrive::run(void)
{
    ESP_UTILS_LOGD("Run(@0x%p)", this);

    /* 框架在 app 关闭时会回收 run() 期间建的屏幕/定时器 ⇒ 上一轮存的指针
     * 此刻已经悬空。**每次进 run() 先清空，再无条件重建**（框架会回收 run() 期间建的资源，不能用"指针非空"当"已创建"）。 */
    _forgetLvObjects();

    lv_obj_t *scr = lv_scr_act();

    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101417), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, TXT_TITLE);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4CD964), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -120);

    lv_obj_t *status = lv_label_create(scr);
    _status = status;
    lv_label_set_text(status, TXT_INIT);
    lv_obj_set_style_text_color(status, lv_color_hex(0xE8EAED), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(status, lv_pct(86));
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint,
                      "The SD card is owned by the PC while this page is open.\n"
                      "Eject the drive on the PC before leaving.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA0A6), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, lv_pct(86));
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *leave = lv_button_create(scr);
    lv_obj_set_size(leave, 220, 64);
    lv_obj_align(leave, LV_ALIGN_CENTER, 0, 130);
    lv_obj_add_event_cb(leave, _onLeaveClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *leave_label = lv_label_create(leave);
    lv_label_set_text(leave_label, "Leave");
    lv_obj_set_style_text_font(leave_label, &lv_font_montserrat_20, 0);
    lv_obj_center(leave_label);

    /* 进页面即切换。首次会重建卡 + 装 USB 栈，耗时几百毫秒（在 LVGL 任务上同步做，
     * 一次性开销，可以接受）。 */
    if (!UsbStorage::getInstance().enterUsbMode()) {
        lv_label_set_text_fmt(status, "Cannot enter USB drive mode:\n%s",
                              UsbStorage::getInstance().lastError());
        lv_obj_set_style_text_color(status, lv_color_hex(0xFF5252), 0);
    }

    /* 状态刷新 + 拔线自动回退。
     * 定时器由框架在 app 关闭时回收（enable_recycle_resource 默认开），
     *    所以**每次 run() 无条件重建**（框架会替我们删，不能用"指针非空"当"已创建"）。 */
    _tick = lv_timer_create(_onTick, 500, this);

    return true;
}

void UsbDrive::_forgetLvObjects(void)
{
    /* 只清不删：框架已经/即将删掉它们，这里再删就是二次释放 */
    _status = nullptr;
    _tick = nullptr;
}

void UsbDrive::_onTick(lv_timer_t *t)
{
    UsbDrive *self = (UsbDrive *)t->user_data;
    UsbStorage &usb = UsbStorage::getInstance();

    /* app 已经关掉了（框架删屏幕/定时器与我们这一拍撞上）⇒ 直接退出，别碰任何 lv 对象 */
    lv_obj_t *status = (self != nullptr) ? self->_status : nullptr;
    if (status == nullptr) {
        return;
    }

    if (usb.active()) {
        if (usb.hostAttached()) {
            self->_ever_attached = true;
            lv_obj_set_style_text_color(status, lv_color_hex(0x4CD964), 0);
            lv_label_set_text(status, "Connected. The SD card is now a USB drive on the PC.");
        } else if (self->_ever_attached) {
            /* PC 那边断开了（多半是拔了线）。把卡还给本地，
             * 否则用户会以为"板子读不到卡了" —— 这是能自愈的部分。 */
            lv_obj_set_style_text_color(status, lv_color_hex(0xFFB300), 0);
            lv_label_set_text(status, "PC disconnected. SD card returned to this device.");
            self->_leaveUsbMode();
            self->_ever_attached = false;
        } else {
            lv_obj_set_style_text_color(status, lv_color_hex(0xE8EAED), 0);
            lv_label_set_text(status,
                              "Waiting for the PC...\nPlug a cable into the USB-C (OTG) port.");
        }
    } else {
        if (self->_ever_attached && usb.enterUsbMode()) {
            /* 拔掉后又插回来：重新把卡让出去（此时只是切挂载点，很快） */
            lv_obj_set_style_text_color(status, lv_color_hex(0x4CD964), 0);
            lv_label_set_text(status, "Reconnected. The SD card is a USB drive again.");
        } else {
            lv_obj_set_style_text_color(status, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text_fmt(status, "Not in USB drive mode.\n%s", usb.lastError());
        }
    }
}

void UsbDrive::_leaveUsbMode(void)
{
    if (!UsbStorage::getInstance().exitUsbMode()) {
        ESP_UTILS_LOGW("Exit USB mode reported: %s", UsbStorage::getInstance().lastError());
    }
}

void UsbDrive::_onLeaveClicked(lv_event_t *e)
{
    UsbDrive *self = (UsbDrive *)lv_event_get_user_data(e);
    ESP_UTILS_CHECK_NULL_EXIT(self, "Invalid user data");

    ESP_UTILS_CHECK_FALSE_EXIT(self->back(), "Back failed");
}

bool UsbDrive::back(void)
{
    ESP_UTILS_LOGD("Back(@0x%p)", this);

    /* 先把卡还给本地，再通知框架关页 —— 顺序反过来的话，
     * 框架销毁屏幕期间 /sdcard 还是摘掉的状态。 */
    _leaveUsbMode();

    _forgetLvObjects();

    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");

    return true;
}

} // namespace esp_brookesia::apps
