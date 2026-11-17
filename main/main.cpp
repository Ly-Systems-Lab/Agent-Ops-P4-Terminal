/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <atomic>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "boost/thread.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

#include "bsp_board_extra.h"

#include "Drawpanel.hpp"
#include "SpecAnalyzer.hpp"
#include "MusicPlayer.hpp"
#include "Settings.hpp"
// ↓ Settings → Display 里的「横竖屏切换」把方向存在 NVS；开机要在注册 display 之前读出来
#include "display_orientation.hpp"
// ↓ 音量 / 亮度持久化：每次开机恢复上次关机前的值
#include "persisted_settings.hpp"
// ↓ 开机自动联网：常驻 WiFi 服务，不许自启网页控制台
#include "brookesia_net.hpp"
#include "brookesia_hub_commands.hpp"
#include "Camera.hpp"
#include "VideoPlayer.hpp"
#include "esp_brookesia_app_calculator.hpp"
#include "XiaozhiApp.hpp"
// ↓ 相册，见 components/brookesia_app_album/
#include "esp_brookesia_app_album.hpp"
// ↓ 聊天/控制面板：当前是本机回环的假后端，见 components/brookesia_app_chat/
#include "esp_brookesia_app_hub.hpp"
// ↓ 网页控制台：手机浏览器访问的 Web 服务端，见 components/brookesia_app_webconsole/
#include "esp_brookesia_app_webconsole.hpp"
// ↓ 服务本体。开机挂载 16 MB 数据分区要用它（见下面 bsp_spiffs_mount 之后）
#include "esp_brookesia_web_console.hpp"
// ↓ U 盘模式：把 SD 卡当 U 盘交给 PC，见 components/brookesia_app_usbdrive/
#include "esp_brookesia_app_usbdrive.hpp"

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

constexpr bool EXAMPLE_SHOW_MEM_INFO = false;

static volatile bool s_wifi_connected = false;

static void wifi_status_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_wifi_connected = false;
    } else if (event_base == WIFI_EVENT &&
               (event_id == WIFI_EVENT_STA_DISCONNECTED || event_id == WIFI_EVENT_STA_STOP)) {
        s_wifi_connected = false;
    }
}

static void register_wifi_status_events()
{
    static bool registered = false;
    if (registered) {
        return;
    }

    esp_err_t ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_status_event_handler, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Register Wi-Fi status handler failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_status_event_handler, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Register IP status handler failed: %s", esp_err_to_name(ret));
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_status_event_handler);
        return;
    }

    registered = true;
}

static bool is_wifi_connected()
{
    return s_wifi_connected;
}

namespace {

class BootLoadingUi {
public:
    static constexpr uint32_t STATUS_UPDATE_PERIOD_MS = 30;
    static constexpr uint32_t READY_HOLD_MS = 300;
    static constexpr uint32_t FADE_OUT_TIME_MS = 240;

    bool create()
    {
        root_ = lv_obj_create(lv_layer_top());
        if (root_ == nullptr) {
            return false;
        }

        spinner_ = lv_spinner_create(root_);
        title_ = lv_label_create(root_);
        phase_ = lv_label_create(root_);
        status_ = lv_label_create(root_);
        progress_ = lv_bar_create(root_);
        if (spinner_ == nullptr || title_ == nullptr || phase_ == nullptr ||
                status_ == nullptr || progress_ == nullptr) {
            destroy();
            return false;
        }

        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
        lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x101417), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_size(spinner_, 64, 64);
        lv_obj_align(spinner_, LV_ALIGN_CENTER, 0, -110);
        lv_obj_set_style_arc_width(spinner_, 5, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner_, 5, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner_, lv_color_hex(0x313B42), LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner_, lv_color_hex(0x27C1A8), LV_PART_INDICATOR);

        lv_label_set_text(title_, "ESP32-P4");
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_32, LV_PART_MAIN);
        lv_obj_set_style_text_color(title_, lv_color_hex(0xF7F9FA), LV_PART_MAIN);
        lv_obj_align(title_, LV_ALIGN_CENTER, 0, -36);

        lv_label_set_text(phase_, "LOADING");
        lv_obj_set_style_text_font(phase_, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(phase_, lv_color_hex(0xF2B84B), LV_PART_MAIN);
        lv_obj_align(phase_, LV_ALIGN_CENTER, 0, 4);

        lv_obj_set_width(status_, lv_pct(84));
        lv_label_set_long_mode(status_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(status_, lv_color_hex(0xA8B1B8), LV_PART_MAIN);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, 44);

        lv_obj_set_size(progress_, lv_pct(52), 7);
        lv_obj_align(progress_, LV_ALIGN_CENTER, 0, 82);
        lv_bar_set_range(progress_, 0, 100);
        lv_obj_set_style_radius(progress_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(progress_, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(progress_, lv_color_hex(0x2A343B), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(progress_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(progress_, lv_color_hex(0x27C1A8), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(progress_, LV_OPA_COVER, LV_PART_INDICATOR);

        update_timer_ = lv_timer_create(onUpdateTimer, STATUS_UPDATE_PERIOD_MS, this);
        if (update_timer_ == nullptr) {
            destroy();
            return false;
        }

        setStageNow("Preparing display...", 8);
        return true;
    }

    void queueStage(const char *status, int progress)
    {
        if (status == nullptr) {
            return;
        }

        if (progress < 0) {
            progress = 0;
        } else if (progress > 100) {
            progress = 100;
        }

        pending_status_.store(status, std::memory_order_relaxed);
        pending_progress_.store(progress, std::memory_order_relaxed);
        pending_revision_.fetch_add(1, std::memory_order_release);
    }

    void showError(const char *status)
    {
        if (root_ == nullptr) {
            return;
        }

        stopUpdateTimer();
        lv_obj_add_flag(spinner_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(phase_, "STARTUP FAILED");
        lv_obj_set_style_text_color(phase_, lv_color_hex(0xF06A6A), LV_PART_MAIN);
        lv_obj_align(phase_, LV_ALIGN_CENTER, 0, 4);
        lv_label_set_text(status_, status);
        lv_obj_set_style_text_color(status_, lv_color_hex(0xF7F9FA), LV_PART_MAIN);
        lv_bar_set_value(progress_, 100, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(progress_, lv_color_hex(0xF06A6A), LV_PART_INDICATOR);
        lv_obj_move_foreground(root_);
    }

    void finish()
    {
        if (root_ == nullptr) {
            return;
        }

        stopUpdateTimer();
        setStageNow("Ready", 100);

        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, root_);
        lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&fade, FADE_OUT_TIME_MS);
        lv_anim_set_delay(&fade, READY_HOLD_MS);
        lv_anim_set_exec_cb(&fade, [](void *object, int32_t opacity) {
            lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), opacity, LV_PART_MAIN);
        });
        lv_anim_set_completed_cb(&fade, lv_obj_delete_anim_completed_cb);
        lv_anim_start(&fade);

        // The animation owns root_ now and deletes it from the LVGL task.
        clearObjectPointers();
    }

    void attachToTopLayer(lv_display_t *display)
    {
        attachToLayer(display != nullptr ? lv_display_get_layer_top(display) : nullptr);
    }

    void attachToSystemLayer(lv_display_t *display)
    {
        attachToLayer(display != nullptr ? lv_display_get_layer_sys(display) : nullptr);
    }

    void destroy()
    {
        stopUpdateTimer();
        if (root_ != nullptr) {
            lv_obj_delete(root_);
        }
        clearObjectPointers();
    }

private:
    static void onUpdateTimer(lv_timer_t *timer)
    {
        auto *self = static_cast<BootLoadingUi *>(lv_timer_get_user_data(timer));
        if (self != nullptr) {
            self->applyPendingStage();
        }
    }

    void applyPendingStage()
    {
        const uint32_t revision = pending_revision_.load(std::memory_order_acquire);
        if (revision == applied_revision_) {
            return;
        }

        const char *status = pending_status_.load(std::memory_order_relaxed);
        const int progress = pending_progress_.load(std::memory_order_relaxed);
        if (status != nullptr) {
            setStageNow(status, progress);
        }
        applied_revision_ = revision;
    }

    void setStageNow(const char *status, int progress)
    {
        if (root_ == nullptr) {
            return;
        }

        lv_label_set_text(status_, status);
        lv_bar_set_value(progress_, progress, LV_ANIM_OFF);
        lv_obj_move_foreground(root_);
    }

    void stopUpdateTimer()
    {
        if (update_timer_ != nullptr) {
            lv_timer_delete(update_timer_);
            update_timer_ = nullptr;
        }
    }

    void clearObjectPointers()
    {
        root_ = nullptr;
        spinner_ = nullptr;
        title_ = nullptr;
        phase_ = nullptr;
        status_ = nullptr;
        progress_ = nullptr;
    }

    void attachToLayer(lv_obj_t *parent)
    {
        if (root_ == nullptr || parent == nullptr) {
            return;
        }

        if (lv_obj_get_parent(root_) != parent) {
            lv_obj_set_parent(root_, parent);
            lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
            lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);
        }
        lv_obj_move_foreground(root_);
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *spinner_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *phase_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *progress_ = nullptr;
    lv_timer_t *update_timer_ = nullptr;
    std::atomic<const char *> pending_status_{nullptr};
    std::atomic<int> pending_progress_{0};
    std::atomic<uint32_t> pending_revision_{0};
    uint32_t applied_revision_ = 0;
};

static void refresh_boot_loading(BootLoadingUi &loading, const char *status, int progress)
{
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Startup stage begin: %s (%d%%)", status, progress);
    loading.queueStage(status, progress);
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Startup stage queued: %s (%d%%)", status, progress);
}

class BootLoadingFailureGuard {
public:
    BootLoadingFailureGuard(BootLoadingUi &loading, lv_display_t *display)
        : loading_(loading), display_(display)
    {
    }

    ~BootLoadingFailureGuard()
    {
        if (!armed_) {
            return;
        }

        LvLockGuard gui_guard;
        loading_.attachToSystemLayer(display_);
        loading_.showError("Unable to finish startup. Please restart the device.");
    }

    void dismiss()
    {
        armed_ = false;
    }

private:
    BootLoadingUi &loading_;
    lv_display_t *display_;
    bool armed_ = true;
};

class PhoneStartupOwner {
public:
    PhoneStartupOwner(Phone *phone, BootLoadingUi &loading, lv_display_t *display)
        : phone_(phone), loading_(loading), display_(display)
    {
    }

    ~PhoneStartupOwner()
    {
        reset();
    }

    PhoneStartupOwner(const PhoneStartupOwner &) = delete;
    PhoneStartupOwner &operator=(const PhoneStartupOwner &) = delete;

    Phone *get() const
    {
        return phone_;
    }

    Phone *release()
    {
        Phone *phone = phone_;
        phone_ = nullptr;
        return phone;
    }

private:
    void reset()
    {
        if (phone_ == nullptr) {
            return;
        }

        if (!bsp_extra_display_lock(-1)) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Lock display while cleaning up phone failed");
            return;
        }
        loading_.attachToTopLayer(display_);
        delete phone_;
        phone_ = nullptr;
        bsp_extra_display_unlock();
    }

    Phone *phone_;
    BootLoadingUi &loading_;
    lv_display_t *display_;
};

} // namespace

/* 复位原因要打**名字**，不能打数字。
 *
 * 为什么值得专门写一段：**"设备自己重启了"是排查里最难的一类** —— 崩溃那一刻的
 * panic 回溯如果没被抓到就永远没了，而复位原因会一直留到下一次启动。
 * 它是唯一能把"正常上电 / 软件复位 / 看门狗 / 掉电 / 锁死"分开的证据。
 * 打数字的话，事后看日志还得去查表（esp_reset_reason_t），容易看漏。
 * （2026-10-13 用户上传中途设备自重启，当时串口没在抓，全程没有证据 —— 就是这次加的。） */
static const char *ResetReasonName(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "POWERON (正常上电)";
    case ESP_RST_EXT:      return "EXT (外部复位脚)";
    case ESP_RST_SW:       return "SW (软件 esp_restart)";
    case ESP_RST_PANIC:    return "PANIC ★ 程序崩溃 —— 看上面的回溯";
    case ESP_RST_INT_WDT:  return "INT_WDT ★ 中断看门狗";
    case ESP_RST_TASK_WDT: return "TASK_WDT ★ 任务看门狗（某任务长时间不让出 CPU）";
    case ESP_RST_WDT:      return "WDT ★ 其它看门狗";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT ★ 掉电/供电不足";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "其它/未知";
    }
}

extern "C" void app_main(void)
{
    esp_log_level_set("rpc_rsp", ESP_LOG_ERROR);

    ESP_UTILS_LOGW("Reset reason: %s (%d)",
                   ResetReasonName(esp_reset_reason()), static_cast<int>(esp_reset_reason()));
    ESP_UTILS_LOGI("Display ESP-Brookesia phone demo");

    /* ========================================================================
     * 屏幕方向（横/竖屏）由 Settings → Display 的按钮控制
     *
     * 方向存在 NVS（`display/rotation`：0 = 竖屏 800x1280，90 = 横屏 1280x800；
     * 键不存在时默认横屏，与之前的行为一致）。见
     * `components/Settings/display_orientation.hpp`。
     *
     * 必须在 `bsp_display_start_with_config()` **之前**读出来：适配器在注册 display
     *    时就把 rotation 算进 LVGL 逻辑分辨率（`display_manager.c:919-922`），并在创建
     *    bridge 时快照进 runtime（`display_bridge_common.c:686`），**没有**运行时改的 API。
     * ⇒ 所以 NVS 初始化从下面的 "Initializing settings..." 提前到这里。
     *
     * 为什么 `rotation` 和 `touch_flags` 必须**一起**改：
     *   适配器把旋转做在 **flush 路径**（PPA 硬件），它**从不调用** `lv_display_set_rotation()`
     *   ⇒ LVGL 自己**不知道**屏幕转了。于是 touch indev 读到的坐标会被 LVGL 原样当成
     *   逻辑坐标用（`esp_lv_adapter_input_touch.c:483-485` 只乘了一个 scale=1，没有任何旋转补偿）。
     *   ⇒ 触摸的旋转**只能**靠 `esp_lcd_touch` 的 flags 来做。
     *
     * 横屏那组 touch_flags 是**反解出来的、不是猜的**（依据：`display_bridge_common.h:139-162`
     * 的 `display_coord_to_phy()`，也就是适配器自己"逻辑→物理"的唯一权威定义；
     * 已核对它真落到硬件上：`lvgl_bridge_v9.c:2843-2847` 把 ROTATE_270 翻成
     * `PPA_SRM_ROTATION_ANGLE_90`，偏移量算的正是同一个式子）：
     *     ROTATE_270 ⇒  phy_x = lv_y ;  phy_y = 1279 - lv_x
     *   反解 ⇒        lv_x = 1279 - ry ;  lv_y = rx    (rx/ry = 面板原生坐标 800x1280)
     *   再套 esp_lcd_touch 的变换顺序(**先 mirror_x → mirror_y，最后 swap_xy**，
     *   `esp_lcd_touch.c:94-115`)：
     *     swap_xy=1（X 取 ry）、mirror_x=0、mirror_y=1（Y = 1280-ry）
     *   ⇒ **swap_xy=1, mirror_x=0, mirror_y=1**
     *   （镜像走的是 `max - x`，与 lv 的 1279-ry 差 1 像素 —— 改造前的横屏也是这么差 1，
     *    触摸分辨率下看不出来。）
     * 竖屏（ROTATE_0）就是面板原生方向 ⇒ 三个 flag 全 0（BSP 出厂默认配置）。
     *
     * 横屏从 ROTATE_90 改成 **ROTATE_270**（整个界面转 180°）。
     *   原因：本板摄像头在面板的**下方**，用户要的横屏是"摄像头朝下"那一侧。
     *   逻辑分辨率仍是 1280x800（`display_manager.c:918-922` 对 90/270 一样对调）
     *   ⇒ 壁纸/相册这些按逻辑分辨率判尺寸的代码一处都不用改。
     * ======================================================================== */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "NVS requires erase before initialization: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "NVS erase failed: %s", esp_err_to_name(err));
            return;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "NVS initialization failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "NVS initialization complete");

    const uint16_t saved_rotation = esp_brookesia::apps::display_orientation::loadRotation();
    const bool landscape = (saved_rotation == esp_brookesia::apps::display_orientation::ROTATION_LANDSCAPE);
    ESP_UTILS_LOGI("Saved screen orientation: %s (%d deg)",
                   esp_brookesia::apps::display_orientation::name(saved_rotation), (int)saved_rotation);

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = landscape ? ESP_LV_ADAPTER_ROTATE_270 : ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {                        /* 见上面的推导 */
            .swap_xy = landscape ? 1u : 0u,
            .mirror_x = 0u,
            .mirror_y = landscape ? 1u : 0u}};

    // Keep LVGL on core 1 with a larger PSRAM-backed stack; high-resolution UI and
    // app switching can otherwise starve the GUI task or overflow the default stack.
    cfg.lv_adapter_cfg.task_stack_size = 16 * 1024;
    cfg.lv_adapter_cfg.task_priority = 10;
    cfg.lv_adapter_cfg.task_core_id = 1;
    cfg.lv_adapter_cfg.stack_in_psram = true;

    /* Configure display */
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    ESP_UTILS_CHECK_NULL_EXIT(disp, "Start display failed");
    bsp_display_backlight_off();

    /* Configure GUI lock */
    LvLock::registerCallbacks([](int timeout_ms) {
        bool locked = bsp_extra_display_lock(timeout_ms);
        ESP_UTILS_CHECK_FALSE_RETURN(locked, false, "Lock failed (timeout_ms: %d)", timeout_ms);

        return true;
    }, []() {
        bsp_extra_display_unlock();
        return true;
    });

    BootLoadingUi boot_loading;
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Startup stage begin: Preparing display... (8%%)");
    {
        LvLockGuard gui_guard;
        if (!boot_loading.create()) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Create boot loading UI failed");
        }
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Startup stage ready: Preparing display... (8%%)");
    // Let the LVGL worker render the first frame before enabling the backlight.
    vTaskDelay(pdMS_TO_TICKS(50));
    /* 恢复上次关机前的亮度（原来写死 `bsp_display_backlight_on()` = 100%）。
     * 顺带修掉一个连带问题：`bsp_display_brightness_get()` 读的就是 `set()` 记下的那个
     * 静态变量，以前开机被写成 100，在 Display 页拖过、切走再回来就弹回 100%；
     * 现在读到的是真值。取值范围 20..100 与 DisplayPage 的滑条一致（越界已在头文件里夹好）。 */
    const int saved_brightness = esp_brookesia::apps::persisted_settings::loadBrightness();
    bsp_display_brightness_set(saved_brightness);
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Restored brightness: %d%%", saved_brightness);

    BootLoadingFailureGuard boot_failure_guard(boot_loading, disp);

    /* NVS 已经在注册 display 之前初始化过了（读屏幕方向要用，见上面）。 */
    refresh_boot_loading(boot_loading, "Initializing settings...", 16);

    refresh_boot_loading(boot_loading, "Mounting storage...", 20);
    err = bsp_spiffs_mount();
    if (err != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "SPIFFS mount successfully");

    /* 16 MB 数据分区（网页控制台上传文件的落点）也在开机时挂上。
     * 刻意放在这里而不是等点开网页控制台才挂：音乐播放器、相册都要读 /data，
     * 否则"重启后不先点网页控制台，就看不到自己上传的东西"。
     * 挂不上不致命 —— 只是那些 app 会看到空目录。 */
    if (!esp_brookesia::services::WebConsole::getInstance().ensureStorageMounted()) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "Data partition mount failed; uploads will be unavailable");
    }

    /* SD 卡：理由和上面的 /data 完全一样，**开机就挂**。音乐播放器 / 相册 / 视频播放器
     * 都要读它，不能让"得先点开某个 app 才认卡"这种事发生。
     * 挂不上不是致命错 —— 只是那几个 app 会少掉卡上那一批素材。
     *
     * 卡要在**开机前**插好。BSP 的 slot 配置用的是 SDMMC_SLOT_NO_CD（板子没有卡检测脚），
     *    所以开机后再插卡不会被发现，得重启板子。这是硬件决定的，不是我们偷懒。
     * 这里先挂了，VideoPlayer 进 app 时还会再调一次 bsp_sdcard_mount()，
     *    所以 **bsp_sdcard_mount() 必须幂等**。
     * 这条原先是靠"IDF 文档写明重复挂载返回 ESP_ERR_INVALID_STATE"推的，**推错了**：
     *       VideoPlayer 调的是 **BSP 的包装函数**，它在调 IDF 之前先申请片上 LDO，
     *       而 LDO 通道只能占一次 ⇒ 第二次返回的是 ESP_ERR_INVALID_ARG ⇒ 屏幕显示 `sd error`。
     *       （2026-10-15 实测，日志见 video Player 那一轮：esp_ldo_acquire_channel 失败。
     *        查返回值语义时必须先确认"拿到这个返回值的是哪一层"。）
     * 现在已在 BSP 里补了幂等（bsp_sdcard != NULL 就返回 ESP_ERR_INVALID_STATE），
     *       与 VideoPlayer.cpp:91 的判断对齐 ⇒ 不会再退化成 sd error。 */
    err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "SD card mount failed (%s); SD contents will be unavailable",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
    }

    refresh_boot_loading(boot_loading, "Starting audio...", 35);
    err = bsp_extra_codec_init();
    if (err != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Codec initialization failed: %s", esp_err_to_name(err));
        return;
    }

    refresh_boot_loading(boot_loading, "Configuring audio...", 45);
    /* 恢复上次关机前的音量（没存过就是出厂默认 CODEC_DEFAULT_VOLUME = 80）。 */
    const int saved_volume = esp_brookesia::apps::persisted_settings::loadVolume();
    err = bsp_extra_codec_volume_set(saved_volume, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "Codec volume setup failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(ESP_UTILS_LOG_TAG, "Restored volume: %d", saved_volume);

    /* Create a phone object */
    refresh_boot_loading(boot_loading, "Building home screen...", 55);
    PhoneStartupOwner phone_owner(new (std::nothrow) Phone(disp), boot_loading, disp);
    Phone *phone = phone_owner.get();
    ESP_UTILS_CHECK_NULL_EXIT(phone, "Create phone failed");

    {
        // When operating on non-GUI tasks, should acquire a lock before operating on LVGL
        LvLockGuard gui_guard;

        /* 判据必须是 **LVGL 的逻辑分辨率**，不是 BSP 的**面板常数**。
         * 这两者在开了旋转（ROTATE_90/270）之后**不再相等**：
         *   面板仍然物理是 800x1280，但 lv_display 被建成了 **1280x800**
         *   （`display_manager.c:918-921` 会按 rotation 把 hor/ver 对调）。
         * 2026-10-16 实测（当时把 rotation 写死 90°）：这里按面板常数选中了 "800_1280"
         * 那套 ⇒ Phone 校准屏幕尺寸时发现 **宽 1280 > 上限 800**，直接拒绝：
         *   E BS:GUIStyle: calibrate(): Invalid value: 1280, should be in range [1, 800]
         *   E BS:Phone:    calibrateScreenSize(): Invalid screen size
         *   E Main:        app_main: Add display stylesheet failed
         * ⇒ 开机停在 "STARTUP FAILED"。**不是崩溃，是框架明确拒绝**（日志见
         *   开发阶段的实测日志）。
         * 上游**本来就有 1280x800 这一套**（`stylesheets/1280_800/`，由
         * `esp_brookesia_phone_stylesheets.hpp:18` 一并 include），只是这里没被选中。
         * ⇒ 按逻辑分辨率选，横竖屏都成立。 */
        const lv_coord_t lv_logical_w = lv_display_get_horizontal_resolution(disp);
        const lv_coord_t lv_logical_h = lv_display_get_vertical_resolution(disp);
        ESP_UTILS_LOGI(ESP_UTILS_LOG_TAG, "Panel %dx%d -> LVGL logical %dx%d",
                       (int)BSP_LCD_H_RES, (int)BSP_LCD_V_RES, (int)lv_logical_w, (int)lv_logical_h);

        if (lv_logical_w == 720 && lv_logical_h == 1280) {
            const auto &target_stylesheet = ESP_BROOKESIA_PHONE_720_1280_DARK_STYLESHEET();
            ESP_UTILS_CHECK_FALSE_EXIT(phone->addStylesheet(target_stylesheet), "Add display stylesheet failed");
            ESP_UTILS_CHECK_FALSE_EXIT(phone->activateStylesheet(target_stylesheet), "Activate display stylesheet failed");
        } else if (lv_logical_w == 800 && lv_logical_h == 1280) {
            const auto &target_stylesheet = ESP_BROOKESIA_PHONE_800_1280_DARK_STYLESHEET();
            ESP_UTILS_CHECK_FALSE_EXIT(phone->addStylesheet(target_stylesheet), "Add display stylesheet failed");
            ESP_UTILS_CHECK_FALSE_EXIT(phone->activateStylesheet(target_stylesheet), "Activate display stylesheet failed");
        } else if (lv_logical_w == 1280 && lv_logical_h == 800) {
            /* 横屏（NVS 里 rotation=90）走这一支 */
            const auto &target_stylesheet = ESP_BROOKESIA_PHONE_1280_800_DARK_STYLESHEET();
            ESP_UTILS_CHECK_FALSE_EXIT(phone->addStylesheet(target_stylesheet), "Add display stylesheet failed");
            ESP_UTILS_CHECK_FALSE_EXIT(phone->activateStylesheet(target_stylesheet), "Activate display stylesheet failed");
        } else {
            ESP_UTILS_LOGE(ESP_UTILS_LOG_TAG, "No compatible display stylesheet for logical %dx%d",
                           (int)lv_logical_w, (int)lv_logical_h);
            return;
        }

        /* Begin the phone */
        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");

        /* ===== 恢复上次设置的桌面壁纸 =====
         * 壁纸是"编码后裁剪成屏幕尺寸"的 RGB565，存在 /data/wallpaper.rgb565；
         * 开机直接读回来挂到主屏上，不依赖原图还在不在（SD 卡拔了也照样有）。
         * 文件不存在 / 尺寸与当前逻辑分辨率不符 / CRC 不对 ⇒ 返回 false，用默认背景。
         * 这里已经持有 LVGL 锁（外层 LvLockGuard），函数内部只做 I/O + 一次样式设置。 */
        esp_brookesia::apps::Album::wallpaperRestore(phone->getDisplay().getMainScreenObject());

        boot_loading.attachToSystemLayer(disp);
        // assert(phone->getDisplay().showContainerBorder() && "Show container border failed");
    }

    /* Init and install apps from registry */
    refresh_boot_loading(boot_loading, "Loading application registry...", 65);
    {
        std::vector<systems::base::Manager::RegistryAppInfo> inited_apps;
        ESP_UTILS_CHECK_FALSE_EXIT(phone->initAppFromRegistry(inited_apps), "Init app registry failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installAppFromRegistry(inited_apps), "Install app registry failed");
    }

    refresh_boot_loading(boot_loading, "Loading tools...", 74);
    {
        auto app2 = esp_brookesia::apps::Calculator::requestInstance();
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(app2), "Start Calculator failed");
        auto app = esp_brookesia::apps::Drawpanel::requestInstance();
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(app), "Start Drawpanel failed");

        auto app1 = esp_brookesia::apps::SpecAnalyzer::requestInstance();
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(app1), "Start SpecAnalyzer failed");
    }

    /* ===== 相册：浏览嵌在固件里的 JPEG ===== */
    {
        // use_navigation_bar = false ⇒ 我们自己画顶栏，不再多加一条系统导航栏
        auto album_app = esp_brookesia::apps::Album::requestInstance(false, false);
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(album_app), "Start Album failed");
    }

    /* ===== 聊天/控制面板=====
     * 桌面上叫 "Hub"。**MQTT 不在这里启动** —— 它归常驻的
     * `services::HubService` 管（见本文件末尾开机流程最后一段）。 */
    {
        auto hub_app = esp_brookesia::apps::Hub::requestInstance(false, false);
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(hub_app), "Start Hub failed");
    }

    /* ===== 网页控制台：手机浏览器进来上传文件的 Web 服务端 =====
     * 进这个 app 才会把服务拉起来；退出 app 不停服务器，回桌面照样能传。 */
    {
        auto web_console_app = esp_brookesia::apps::WebConsoleApp::requestInstance(false, false);
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(web_console_app), "Start WebConsole failed");
    }

    /* ===== U 盘模式：把 SD 卡当 U 盘交给 PC =====
     * 只注册，不动 SD —— 真正切换发生在用户点进这个 app 的时候
     * （见 components/brookesia_app_usbdrive/ 与组件 brookesia_usb_storage）。 */
    {
        auto usb_drive_app = esp_brookesia::apps::UsbDrive::requestInstance(false, false);
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(usb_drive_app), "Start USB Drive failed");
    }

    refresh_boot_loading(boot_loading, "Loading media...", 82);
    {
        auto app5 = esp_brookesia::apps::MusicPlayer::requestInstance();
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(app5), "Start MusicPlayer failed");
        auto camera_app = esp_brookesia::apps::Camera::requestInstance();
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(camera_app), "Start Camera failed");
        auto video_player_app = esp_brookesia::apps::VideoPlayer::requestInstance();
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(video_player_app), "Start VideoPlayer failed");
    }

    refresh_boot_loading(boot_loading, "Loading system applications...", 90);
    {
        auto settings_app = esp_brookesia::apps::Settings::requestInstance(false, false);
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(settings_app), "Start Settings failed");
        auto xiaozhi_app = esp_brookesia::apps::XiaozhiApp::requestInstance();
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installApp(xiaozhi_app), "Start Xiaozhi failed");
    }

    refresh_boot_loading(boot_loading, "Finishing setup...", 96);

    register_wifi_status_events();

    {
        LvLockGuard gui_guard;

        /* Create a timer to update the clock */
        lv_timer_t *status_timer = lv_timer_create([](lv_timer_t *t) {
            time_t now;
            struct tm timeinfo;
            Phone *phone = (Phone *)t->user_data;

            ESP_UTILS_CHECK_NULL_EXIT(phone, "Invalid phone");

            time(&now);
            localtime_r(&now, &timeinfo);

            ESP_UTILS_CHECK_FALSE_EXIT(
                phone->getDisplay().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min),
                "Refresh status bar failed"
            );

            bool connected = is_wifi_connected();
            StatusBar::WifiState wifi_state = connected ?
                StatusBar::WifiState::SIGNAL_3 :
                StatusBar::WifiState::DISCONNECTED;

            ESP_UTILS_CHECK_FALSE_EXIT(
                phone->getDisplay().getStatusBar()->setWifiIconState(wifi_state),
                "Refresh status bar failed"
            );
        }, 1000, phone);
        ESP_UTILS_CHECK_NULL_EXIT(status_timer, "Create status timer failed");

        /* ===== MQTT 反控命令的执行器 =====
         * 位置不是随便挑的：
         *   ① 在 **LVGL 锁内** —— 它内部要 `lv_timer_create`（往 LVGL 全局链表插一项）；
         *   ② 在**所有 app 都装完之后** —— 它按 app **名字**去 `getInstalledApp()` 里找 id；
         *   ③ 在 app 的 `run()` 之外 —— 否则框架的 `enable_recycle_resource` 会顺手把它删掉。
         * 命令从 `HubService`（MQTT）进来，与 Hub app 开不开无关。 */
        if (!esp_brookesia::services::HubCommands::getInstance().begin(phone)) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "HubCommands 初始化失败 ⇒ MQTT 命令不会被执行");
        }

        phone_owner.release();
        boot_failure_guard.dismiss();
        // Keep the completed state visible briefly, then reveal the home screen smoothly.
        boot_loading.finish();
    }

    /* ===== 开机自动联网 =====
     * 放在开机流程**最后**：桌面已经出来了，`esp_wifi_init/start` 那点阻塞
     * 不会拖慢启动画面（LVGL 在自己的任务上，不受影响）。
     *
     * 这里**只起 WiFi**，不建 httpd、不挂 /data —— 网页控制台仍然是「点 app 才起」。
     * 连接是异步的：拿到 IP 后由 `NetService` 的事件回调更新状态，
     * 状态栏那 1 Hz 的定时器（上面）会自己反映出来。
     * 是否联网受「设置 → WLAN」那个开关约束（NVS storage/wifi_en）。 */
    if (!esp_brookesia::services::NetService::getInstance().start()) {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "NetService did not start (wifi_en off or init failed)");
    }

    /* ===== MQTT 常驻 =====
     * 接在 WiFi 之后：服务内部会等拿到 IP（最多 60 s）再建 TLS 客户端，
     * **不阻塞这里**（它起一个短命任务）。关掉 Hub app 也不会断线 ——
     * app 只是视图，回执在服务里缓冲。
     * 没配 Pi / 缺证书 / WiFi 被关 ⇒ 返回 false，但服务仍算"就绪"，
     * 之后用户配了地址（保存设置会调 `restart()`）或在设置里开了 WiFi，就能连上。 */
    if (!esp_brookesia::services::HubService::getInstance().start()) {
        ESP_LOGI(ESP_UTILS_LOG_TAG, "HubService: MQTT not started (unconfigured / no certs / wifi off)");
    }

    if constexpr (EXAMPLE_SHOW_MEM_INFO) {
        esp_utils::thread_config_guard thread_config({
            .name = "mem_info",
            .stack_size = 4096,
        });
        boost::thread([ = ]() {
            char buffer[128];    /* Make sure buffer is enough for `sprintf` */
            size_t internal_free = 0;
            size_t internal_total = 0;
            size_t external_free = 0;
            size_t external_total = 0;

            while (1) {
                internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                snprintf(
                    buffer,
                    sizeof(buffer),
                    "\t           Biggest /     Free /    Total\n"
                    "\t  SRAM : [%8zu / %8zu / %8zu]\n"
                    "\t PSRAM : [%8zu / %8zu / %8zu]",
                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), internal_free, internal_total,
                    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), external_free, external_total
                );
                ESP_UTILS_LOGI("\n%s", buffer);

                {
                    LvLockGuard gui_guard;
                    ESP_UTILS_CHECK_FALSE_EXIT(
                        phone->getDisplay().getRecentsScreen()->setMemoryLabel(
                            internal_free / 1024, internal_total / 1024, external_free / 1024, external_total / 1024
                        ), "Set memory label failed"
                    );
                }

                boost::this_thread::sleep_for(boost::chrono::seconds(5));
            }
        }).detach();
    }
}
