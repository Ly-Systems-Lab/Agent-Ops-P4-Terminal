#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"
namespace esp_brookesia::apps {
    class Settings;
}
namespace esp_brookesia::apps {

class DisplayPage : public systems::phone::App {
public:
    static DisplayPage *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    DisplayPage(bool use_status_bar, bool use_navigation_bar);
    virtual ~DisplayPage();

    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void event_handler_cb(lv_event_t *e);
    /* 横竖屏切换行。点一下 = 写 NVS → 0.8 s 后 esp_restart()。 */
    static void on_orientation_clicked_cb(lv_event_t *e);
    static void restart_timer_cb(lv_timer_t *t);

    static DisplayPage *_instance;
    lv_obj_t *page_root;
    lv_obj_t *label;
    lv_obj_t *list1;
    /* 横竖屏那一行右侧的值（Landscape / Portrait）。框架会在关页时删掉它，
     * 这里只做空指针守卫，不负责释放。 */
    lv_obj_t *orientation_value = nullptr;
    lv_style_t style_list;
    lv_style_t style_list_btn;
    lv_style_t style_list_text;
    lv_style_t style_list_btn_pressed;
};

} // namespace esp_brookesia::apps
