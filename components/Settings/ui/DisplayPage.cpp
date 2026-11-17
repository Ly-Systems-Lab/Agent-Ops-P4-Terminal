#include "DisplayPage.hpp"
#include "SettingsUI.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "../display_orientation.hpp"
#include "../persisted_settings.hpp"
#include "../Settings.hpp"

#define ESP_UTILS_LOG_TAG "BS:DisplayPage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps
{
    DisplayPage *DisplayPage::_instance = nullptr;

    DisplayPage *DisplayPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new DisplayPage(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    DisplayPage::DisplayPage(bool use_status_bar, bool use_navigation_bar)
        : App("Display", nullptr, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), label(nullptr), list1(nullptr), orientation_value(nullptr)
    {
    }

    DisplayPage::~DisplayPage()
    {
    }

    bool DisplayPage::run()
    {
        ESP_UTILS_LOGD("DisplayPage Run");
        lv_obj_clean(lv_scr_act());

        page_root = settings_ui::create_page(lv_scr_act());
        settings_ui::create_header(page_root, "Display", [](lv_event_t *e) {
            (void)e;
            lv_async_call([](void *param) {
                (void)param;
                Settings::requestInstance()->showRootPage();
            }, nullptr);
        });

        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Screen", style_list_text);

        lv_obj_t *slider_container = lv_obj_create(list1);
        lv_obj_add_style(slider_container, &style_list_btn, LV_PART_MAIN);
        lv_obj_set_size(slider_container, lv_pct(100), settings_ui::CONTROL_PANEL_HEIGHT);
        lv_obj_set_style_pad_all(slider_container, 20, LV_PART_MAIN);
        lv_obj_clear_flag(slider_container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(slider_container, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(slider_container);
        lv_label_set_text(title, "Brightness");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(settings_ui::COLOR_PRIMARY_TEXT), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

        int32_t value = bsp_display_brightness_get();
        if (value < 20) {
            value = 20;
        } else if (value > 100) {
            value = 100;
        }

        label = lv_label_create(slider_container);
        lv_label_set_text_fmt(label, "%ld%%", static_cast<long>(value));
        lv_obj_set_style_text_font(label, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(settings_ui::COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, 1);

        lv_obj_t *brightness_slider = lv_slider_create(slider_container);
        lv_obj_set_width(brightness_slider, lv_pct(100));
        lv_obj_set_height(brightness_slider, 14);
        lv_obj_align(brightness_slider, LV_ALIGN_BOTTOM_MID, 0, -5);
        lv_slider_set_range(brightness_slider, 20, 100);
        lv_slider_set_value(brightness_slider, value, LV_ANIM_OFF);

        lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0x505050), LV_PART_MAIN);
        lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(settings_ui::COLOR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(brightness_slider, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(brightness_slider, 7, LV_PART_KNOB);
        lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        /* 拖动时收 VALUE_CHANGED（实时改亮度 + 改数字）；
         * 松手再收 RELEASED —— 只有那一刻才写 NVS（见 event_handler_cb）。
         *
         * 必须注册**两次**。LVGL 9 的 filter 只做精确匹配
         * （`lv_event.c:131-132`：`filter == LV_EVENT_ALL || filter == e->code`），
         * 写成 `VALUE_CHANGED | RELEASED` 会按位或出一个不存在的码（35|11=43），
         * 两个事件都匹配不上 ⇒ **回调静默地一次都不执行**（滑条照样能拖，但什么都不生效）。 */
        lv_obj_add_event_cb(brightness_slider, event_handler_cb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(brightness_slider, event_handler_cb, LV_EVENT_RELEASED, this);

        /* ===== 横竖屏切换 =====
         * 适配器没有运行时改旋转的 API（rotation 在注册 display 时就被快照了）⇒
         * 这一行只负责「写 NVS + 重启」，真正生效在 main.cpp 开机那一段。
         * 文案必须 ASCII（montserrat 没有中文字形）。 */
        settings_ui::add_section(list1, "Orientation", style_list_text);

        lv_obj_t *orientation_row = lv_list_add_button(list1, nullptr, "Screen orientation");
        lv_obj_add_style(orientation_row, &style_list_btn, LV_PART_MAIN);
        lv_obj_add_style(orientation_row, &style_list_btn_pressed, LV_STATE_PRESSED);
        settings_ui::use_ellipsis_for_button_label(orientation_row);
        lv_obj_add_event_cb(orientation_row, on_orientation_clicked_cb, LV_EVENT_CLICKED, this);

        orientation_value = lv_label_create(orientation_row);
        lv_label_set_text(orientation_value,
                          display_orientation::name(display_orientation::loadRotation()));
        lv_obj_set_style_text_font(orientation_value, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_set_style_text_color(orientation_value,
                                    lv_color_hex(settings_ui::COLOR_ACCENT), LV_PART_MAIN);
        return true;
    }

    void DisplayPage::event_handler_cb(lv_event_t *e)
    {
        DisplayPage *instance = static_cast<DisplayPage *>(lv_event_get_user_data(e));
        lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
        if (instance == nullptr || slider == nullptr || !lv_obj_has_class(slider, &lv_slider_class)) {
            return;
        }

        const int32_t value = lv_slider_get_value(slider);
        if (instance->label != nullptr) {
            lv_label_set_text_fmt(instance->label, "%ld%%", static_cast<long>(value));
        }
        bsp_display_brightness_set(value);

        /* 只在**松手**时落盘。拖动时 VALUE_CHANGED 是每帧都来的，
         *    逐帧 nvs_commit() 就是白白磨损 flash（而且会卡 LVGL 任务）。
         * 点滑条空白处也会走 RELEASED，所以点选同样能存住。 */
        if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
            esp_brookesia::apps::persisted_settings::saveBrightness(static_cast<int>(value));
        }
    }

    void DisplayPage::on_orientation_clicked_cb(lv_event_t *e)
    {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
            return;
        }

        DisplayPage *self = static_cast<DisplayPage *>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }

        const uint16_t current = display_orientation::loadRotation();
        const uint16_t next = (current == display_orientation::ROTATION_LANDSCAPE)
                              ? display_orientation::ROTATION_PORTRAIT
                              : display_orientation::ROTATION_LANDSCAPE;

        if (!display_orientation::saveRotation(next)) {
            ESP_UTILS_LOGE("Save screen orientation failed");
            if (self->orientation_value != nullptr) {
                lv_label_set_text(self->orientation_value, "Save failed");
                lv_obj_set_style_text_color(self->orientation_value,
                                            lv_color_hex(0xFF5252), LV_PART_MAIN);
            }
            return;
        }

        ESP_UTILS_LOGI("Screen orientation -> %s (%d deg); restarting",
                       display_orientation::name(next), static_cast<int>(next));

        if (self->orientation_value != nullptr) {
            lv_label_set_text_fmt(self->orientation_value, "Restarting: %s",
                                  display_orientation::name(next));
            lv_obj_set_style_text_color(self->orientation_value,
                                        lv_color_hex(settings_ui::COLOR_ACCENT), LV_PART_MAIN);
        }

        /* 不能用 vTaskDelay：回调跑在 LVGL 任务里，睡下去会把「Restarting...」这行字
         *    一起冻住（还没画出来）。起一个一次性 lv_timer，让这一帧先渲染出来再重启。
         *    800 ms 是"看得见"和"不磨蹭"的折中。 */
        lv_timer_t *timer = lv_timer_create(restart_timer_cb, 800, nullptr);
        if (timer != nullptr) {
            lv_timer_set_repeat_count(timer, 1);
        } else {
            esp_restart();
        }
    }

    void DisplayPage::restart_timer_cb(lv_timer_t *t)
    {
        (void)t;
        ESP_UTILS_LOGI("Restarting for screen orientation change");
        esp_restart();
    }

    bool DisplayPage::back()
    {
        ESP_UTILS_LOGD("DisplayPage Back");
        Settings::requestInstance()->showRootPage();
        return true;
    }

    bool DisplayPage::close()
    {
        ESP_UTILS_LOGD("DisplayPage Close");
        if (page_root != nullptr)
        {
            lv_obj_del(page_root);
            page_root = nullptr;
            label = nullptr;
            list1 = nullptr;
            orientation_value = nullptr;
            settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        }
        return true;
    }

} // namespace esp_brookesia::apps
