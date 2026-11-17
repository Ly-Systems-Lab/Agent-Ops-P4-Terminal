/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:App:Hub"
#include "esp_lib_utils.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "lvgl.h"

#include "cbin_font.h"
#include "pinyin_dict_zh.h"

#include "chat_history.hpp"
#include "esp_brookesia_app_hub.hpp"

using namespace esp_brookesia::systems;
using namespace esp_brookesia::apps::chat;

#define APP_NAME "Hub"

/* 桌面图标：112×112 ARGB8888，alpha 掩码借自原厂图标（与 Camera/Settings/AIChats 逐像素一致）。
 * 由 `tools/make_app_icon.py` 从 tb04.jpg 生成，**别手改**。 */
LV_IMAGE_DECLARE(img_app_hub);

namespace esp_brookesia::apps {

namespace {

/* 20 px 中文（`main/CMakeLists.txt` 的 CHAT_COMMON_FONT 把它 staged 到 storage）。
 * 和 AIChats 的 30 px（/spiffs/xiaozhi/font.bin）是**两份**，别互相替换。 */
constexpr char FONT_PATH[] = "/spiffs/fonts/font_puhui_20_4.bin";
constexpr size_t FONT_MAX_BYTES = 2 * 1024 * 1024;

constexpr uint32_t COLOR_BG    = 0x0E1114;
constexpr uint32_t COLOR_USER  = 0x2D7FF9;
constexpr uint32_t COLOR_AGENT = 0x1E252B;
constexpr uint32_t COLOR_ERROR = 0x8C2F28;
constexpr uint32_t COLOR_TEXT  = 0xE8EAED;
constexpr uint32_t COLOR_HINT  = 0x8A9199;

/* 气泡最宽占列表的多少。留 1/4 空白，视线才能一眼分出左右。 */
constexpr int BUBBLE_WIDTH_PCT = 74;
constexpr int KEYBOARD_HEIGHT_PCT = 42;

/* 拼音候选栏的固定高度。默认是 `LV_PCT(5)`（横竖屏会算出不同像素），
 * 改成定值才能让"列表要给键盘+候选栏让出多少"是个确定的数。 */
constexpr int CANDIDATE_PANEL_HEIGHT = 56;

constexpr uint32_t POLL_PERIOD_MS = 120;

/* 「清空」的二次确认窗口：第一下变红等这么久，第二下才真清。
 * 用已有的 120 ms 轮询来复原，**不另建 lv_timer**（框架会替我们删掉 run() 期间建的定时器，自己再管一遍就是删除两次）。 */
constexpr uint32_t CLEAR_CONFIRM_MS = 3000;

/* ---- 连接设置浮层 ---- */
constexpr int CONFIG_CARD_WIDTH_PCT = 72;
constexpr int CONFIG_ROW_HEIGHT = 54;
constexpr int CONFIG_LABEL_WIDTH = 96;
constexpr uint32_t COLOR_OK = 0x6FCF97;
constexpr uint32_t COLOR_BAD = 0xFF8A80;

/* TCP 探活：只证明"这台机器这个端口能连上"，不做 TLS 握手、也不做 MQTT CONNECT
 * （真正的握手在常驻服务的 `MqttTransport` 里，跟这个按钮无关）。
 * 所以它**测不出账号密码对不对** —— UI 文案必须说清楚。 */
constexpr int TEST_CONNECT_TIMEOUT_MS = 2000;
constexpr uint32_t TEST_TASK_STACK = 6144;

/* ==========================================================================
 * 键盘右下角那一格：把 "√" 换成"收起键盘"（↓）
 * ==========================================================================
 *
 * 为什么非改键位表不可：`lv_keyboard` 自己按**按钮文本**分派 ——
 *   - "√"（LV_SYMBOL_OK）→ 先后给键盘和 textarea 发 LV_EVENT_READY；
 *   - "回车"（LV_SYMBOL_NEW_LINE）→ 单行 textarea 上同样发 LV_EVENT_READY。
 * 两条路最后都汇进我们的 `_onReady()` ⇒ **逻辑完全重复**，而且在回调里分不出是谁按的。
 * `lv_buttonmatrix` 又**没有**单键改文本的 API（只有整张 map 替换），所以只能整张换。
 *
 * 下面这张控制位表**逐字抄自** `lvgl/src/widgets/keyboard/lv_keyboard.c:108-117`
 * （`LV_KB_BTN(w)` 在那边是私有宏，等于 `LV_BUTTONMATRIX_CTRL_POPOVER | w`）。
 * 键位表本身不抄 —— 运行期用 `lv_keyboard_get_map_array()` 读默认表再只替换一格，
 * 这样 `LV_KEYBOARD_CTRL_BUTTON_MODE_*` 这些私有常量不用我们复刻。
 * 升 LVGL 后要回头核对这份宽度表；坏掉是"看得见的布局错乱"，不会静默。
 */
/* 默认 TEXT_LOWER 的键位表实际有 **44 项**：
 *   行1 13（模式键 + 10 字母 + 退格 + "\n"）
 *   行2 12（大小写键 + 9 字母 + 回车 + "\n"）
 *   行3 13（12 符号 + "\n"）
 *   行4  6（键盘键 + ← + 空格 + → + ↓ + 终止 ""）
 * 这里**必须留够**：第一版我按 24 留，复制的副本在第 23 项（正好是行 2 的"回车"）
 *    就被 nullptr 提前终止了 ⇒ 行 3/4 整个消失，屏幕上只剩 2 行。
 *    按当时数量写死的常量会在素材变多时静默截断 —— 所以下面还加了一条超限告警。 */
constexpr int KB_MAP_SLOTS = 48;

/* LVGL 把 ctrl 定义成强类型枚举 ⇒ 位或的结果是 int，**每一项都要显式转回来**
 *    （C++ 下枚举不隐式接受 int，位或的结果必须显式转回来）。 */
#define HUB_KB_CTRL(v) ((lv_buttonmatrix_ctrl_t)(v))
#define HUB_KB_BTN(w)  HUB_KB_CTRL(LV_BUTTONMATRIX_CTRL_POPOVER | (w))
#define HUB_KB_BTN_CHK(w) \
    HUB_KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | LV_BUTTONMATRIX_CTRL_POPOVER | (w))

const lv_buttonmatrix_ctrl_t s_kb_ctrl_text[] = {
    /* 行1：模式键 + 10 个字母 + 退格 */
    HUB_KB_CTRL(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 5), HUB_KB_BTN(4), HUB_KB_BTN(4), HUB_KB_BTN(4),
    HUB_KB_BTN(4), HUB_KB_BTN(4), HUB_KB_BTN(4), HUB_KB_BTN(4), HUB_KB_BTN(4),
    HUB_KB_BTN(4), HUB_KB_BTN(4), HUB_KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 7),
    /* 行2：大小写键 + 9 个字母 + 回车 */
    HUB_KB_CTRL(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6), HUB_KB_BTN(3), HUB_KB_BTN(3), HUB_KB_BTN(3),
    HUB_KB_BTN(3), HUB_KB_BTN(3), HUB_KB_BTN(3), HUB_KB_BTN(3), HUB_KB_BTN(3),
    HUB_KB_BTN(3), HUB_KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 7),
    /* 行3：12 个符号 */
    HUB_KB_BTN_CHK(1), HUB_KB_BTN_CHK(1),
    HUB_KB_BTN(1), HUB_KB_BTN(1), HUB_KB_BTN(1), HUB_KB_BTN(1), HUB_KB_BTN(1),
    HUB_KB_BTN(1), HUB_KB_BTN(1), HUB_KB_BTN_CHK(1),
    HUB_KB_BTN_CHK(1), HUB_KB_BTN_CHK(1),
    /* 行4：键盘键 + 左右 + 空格 + 我们那一格（↓） */
    HUB_KB_CTRL(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
    HUB_KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 2),
    HUB_KB_CTRL(6),
    HUB_KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 2),
    HUB_KB_CTRL(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
};

/* 必须静态存储期：`lv_keyboard_set_map()` 只存指针，之后每次重绘都读它（只存指针，不拷贝那块内存）。 */
const char *s_kb_map_lower[KB_MAP_SLOTS];
const char *s_kb_map_upper[KB_MAP_SLOTS];

/** 读当前模式的默认键位表，把 "√" 那格换成 ↓，存进 `dst` 并装回去。 */
void buildKeyMap(lv_obj_t *kb, lv_keyboard_mode_t mode, const char **dst)
{
    lv_keyboard_set_mode(kb, mode);
    const char *const *src = lv_keyboard_get_map_array(kb);

    int n = 0;
    bool terminated = false;
    int replaced = 0;
    int already_down = 0;
    for (; n < KB_MAP_SLOTS - 1; ++n) {
        const char *text = src[n];
        if (text == nullptr) {
            break;
        }
        if (strcmp(text, LV_SYMBOL_OK) == 0) {
            dst[n] = LV_SYMBOL_DOWN;
            ++replaced;
        } else {
            dst[n] = text;
            if (strcmp(text, LV_SYMBOL_DOWN) == 0) {
                ++already_down;
            }
        }
        if (text[0] == '\0') {
            ++n;
            terminated = true;
            break;
        }
    }
    for (; n < KB_MAP_SLOTS; ++n) {
        dst[n] = nullptr;
    }

    /* 别让截断静默发生：副本没读到终止的 "" 就是槽位不够了，
     *    表现是"键盘少几行"，很容易被当成布局问题查半天。 */
    if (!terminated) {
        ESP_LOGE(ESP_UTILS_LOG_TAG, "键位表副本被截断（槽位 %d 不够），键盘会缺行", KB_MAP_SLOTS);
    }
    /* `lv_keyboard` 把键位表存在**文件级静态数组 `kb_map[mode]`** 里（`set_map` 直接写它），
     *    而那个数组**不随键盘对象销毁**。所以第二把键盘进来时，读到的已经是本函数上一轮
     *    装回去的表 —— 里面本来就没有 "√" 了。
     *    ⇒ 这时 `replaced == 0` 且 `already_down == 1` 是**正常状态**，不是异常；
     *      只有"既没替换、也没有 ↓"才说明上游换了符号，那时才该喊。 */
    if (replaced != 1 && !(replaced == 0 && already_down == 1)) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "期望替换 1 个 \"√\"，实际替换了 %d 个（已有 ↓ %d 个）",
                 replaced, already_down);
    }

    lv_keyboard_set_map(kb, mode, dst, s_kb_ctrl_text);
}

} // namespace

Hub *Hub::_instance = nullptr;

Hub *Hub::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new Hub(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

// 桌面图标：`&img_app_hub`（App 的第二个参数要的是 lv_image_dsc_t 的地址，与 AIChats 同姿势）。
// use_default_screen = true ⇒ 在 lv_scr_act() 上画，退出时框架负责回收。
Hub::Hub(bool use_status_bar, bool use_navigation_bar) :
    App(APP_NAME, &img_app_hub, true, use_status_bar, use_navigation_bar)
{
}

Hub::~Hub()
{
    ESP_UTILS_LOGD("Destroy(@0x%p)", this);
}

/* ==========================================================================
 * 字体
 * ========================================================================== */

bool Hub::_ensureFont()
{
    if (_font != nullptr) {
        return true;
    }

    FILE *file = fopen(FONT_PATH, "rb");
    if (file == nullptr) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "字库不可用：%s", FONT_PATH);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long file_size = ftell(file);
    if (file_size <= 0 || static_cast<size_t>(file_size) > FONT_MAX_BYTES ||
            fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "字库大小异常：%ld", file_size);
        fclose(file);
        return false;
    }

    const size_t size = static_cast<size_t>(file_size);
    auto *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "PSRAM 不足以放字库（%u B）", static_cast<unsigned>(size));
        fclose(file);
        return false;
    }

    const size_t got = fread(data, 1, size, file);
    fclose(file);
    if (got != size) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "字库读取不完整（%u/%u）",
                 static_cast<unsigned>(got), static_cast<unsigned>(size));
        heap_caps_free(data);
        return false;
    }

    /* 这里已经在 LVGL 任务里（run() 由框架在 LVGL 上下文调用），不用再上锁 ——
     * 和 XiaozhiUi 不同，它那次是在自己的上下文里加载的。 */
    lv_font_t *cbin = cbin_font_create(data);
    if (cbin == nullptr) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "字库解码失败");
        heap_caps_free(data);
        return false;
    }

    /* 必须挂 fallback：拼音候选栏的翻页键是 LV_SYMBOL_LEFT/RIGHT，
     *    cbin 字库里没有这些符号字形，不挂就是两个方框。
     *    （`lv_font.c:105-125` 会沿 `fallback` 链递归查缺字。） */
    cbin->fallback = &lv_font_montserrat_20;

    _font_data = data;
    _font = cbin;
    ESP_UTILS_LOGI("已加载 20 px 中文字库（%u B，占 PSRAM）", static_cast<unsigned>(size));
    return true;
}

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

bool Hub::run(void)
{
    ESP_UTILS_LOGD("Run(@0x%p)", this);

    /* 上一轮退出时框架已经把 lv 对象删了，指针必须在这里清掉，否则就是用后释放。 */
    _forgetLvObjects();
    {
        std::lock_guard<std::mutex> lock(_pending_mutex);
        _pending.clear();
    }

    if (!_ensureFont()) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "中文字库加载失败，退化为内置字体（中文会显示为方框）");
    }
    if (_font == nullptr) {
        _font = &lv_font_montserrat_20;
    }

    /* 设置浮层要从这里回填，所以每次进来都重新读一遍 NVS
     * （服务那边可能因为别的原因重建过连接）。 */
    _cfg = configLoad();
    ESP_UTILS_LOGI("树莓派配置：%s（服务状态 %d）",
                   _cfg.isConfigured() ? _cfg.summary().c_str() : "未配置",
                   static_cast<int>(services::HubService::getInstance().state()));

    /* MQTT 归常驻服务管，这里**只注册回调，不启停任何东西**。
     * 服务给出的已经是"累积全文"的快照 ⇒ app 中途打开也能显示完整句子。
     * 回调跑在 esp-mqtt 任务里 —— 只塞队列，绝不碰 lv 对象。 */
    services::HubService::getInstance().setReplyHandler([this](const services::ChatReply &reply) {
        std::lock_guard<std::mutex> lock(_pending_mutex);
        if (_pending.size() >= 64) {
            _pending.erase(_pending.begin());
        }
        _pending.push_back(reply);
    });

    _buildUi(lv_scr_act());
    _renderHistory();
    /* app 没打开期间完成的回复，补写进历史和界面（服务的未读环形缓冲）。 */
    _drainUnread();

    _poll_timer = lv_timer_create(_onPoll, POLL_PERIOD_MS, this);
    ESP_UTILS_CHECK_NULL_RETURN(_poll_timer, false, "Create poll timer failed");

    return true;
}

bool Hub::back(void)
{
    ESP_UTILS_LOGD("Back(@0x%p)", this);
    /* 先把回调摘掉：这之后到达的回复进服务的未读缓冲，下次进来补上。
     * `Hub` 是单例、`this` 不会失效，但也别留悬着的回调（借出去的回调必须主动摘掉，不能指望对方自己放手）。 */
    services::HubService::getInstance().setReplyHandler(nullptr);
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool Hub::close(void)
{
    ESP_UTILS_LOGD("Close(@0x%p)", this);
    services::HubService::getInstance().setReplyHandler(nullptr);
    _forgetLvObjects();
    return true;
}

void Hub::_forgetLvObjects(void)
{
    /* 只清不删 —— `enable_recycle_resource` 已经替我们删过了，
     *    再 del 就是二次释放。 */
    _root = nullptr;
    _list = nullptr;
    _empty_hint = nullptr;
    _ta = nullptr;
    _kb_ime = nullptr;
    _ime = nullptr;
    _cand_panel = nullptr;
    _kb_ascii = nullptr;
    _btn_mode = nullptr;
    _poll_timer = nullptr;
    _badge = nullptr;
    _btn_cfg = nullptr;
    _btn_clear = nullptr;
    /* 对象没了，确认窗口也必须作废：下次 run() 重建的按钮是全新的，
     * 不能继承上一次"已经确认过"的状态。 */
    _clear_armed_until = 0;

    /* 浮层及其内部控件同样是框架替我们删的 —— 这里只清指针。 */
    _config_overlay = nullptr;
    _cfg_host = nullptr;
    _cfg_port = nullptr;
    _cfg_user = nullptr;
    _cfg_pass = nullptr;
    _cfg_tls = nullptr;
    _cfg_msg = nullptr;

    /* 这些 label 指针也随对象一起失效了。 */
    _inflight.clear();
    /* 下一次 run() 会重建顶栏，缓存的文字必须一起清掉，否则 `_updateBadge()` 会
     * 认为"没变"而跳过写入，新 label 停在创建时的默认文字。 */
    _badge_text.clear();
}

/* ==========================================================================
 * 构建 UI
 * ========================================================================== */

void Hub::_buildUi(lv_obj_t *scr)
{
    const lv_coord_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    const lv_coord_t screen_h = lv_display_get_vertical_resolution(nullptr);
    _kb_height = screen_h * KEYBOARD_HEIGHT_PCT / 100;
    _cand_height = CANDIDATE_PANEL_HEIGHT;

    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    /* 让没显式设字体的子孙（键盘按钮、候选栏…）默认就能画中文 */
    lv_obj_set_style_text_font(scr, _font, 0);

    _root = lv_obj_create(scr);
    lv_obj_remove_style_all(_root);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_size(_root, screen_w, screen_h);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_root, 14, 0);
    lv_obj_set_style_pad_row(_root, 10, 0);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ---------------- 顶栏 ---------------- */
    lv_obj_t *header = lv_obj_create(_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 44);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Hub");
    lv_obj_set_style_text_font(title, _font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);

    /* 右侧一组：连接状态 + 设置入口。
     * 放这里而不是系统「设置」里：这是这份配置**唯一的使用者**（Hub 的传输层），
     * 而且它天然就在"连没连上"那行字旁边，改完立刻能对照。 */
    lv_obj_t *right = lv_obj_create(header);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 12, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    _badge = lv_label_create(right);
    lv_label_set_text(_badge, "未配置 Pi");
    lv_obj_set_style_text_font(_badge, _font, 0);
    lv_obj_set_style_text_color(_badge, lv_color_hex(COLOR_HINT), 0);

    /* 「清空」—— 卡顿的自救按钮。
     * 只清**本机**：界面气泡 + `/data/chat/history.jsonl`。
     * 绝不碰 Agent 的上下文 —— 树莓派那边的会话状态在 Pi 上，这里删的是
     *    "屏幕上显示过什么"的记录，删了不代表让 Agent 忘掉任何东西。
     * 两下确认：见 `_clearHistory()`。 */
    _btn_clear = lv_button_create(right);
    lv_obj_set_size(_btn_clear, 128, 38);
    lv_obj_set_style_radius(_btn_clear, 12, 0);
    lv_obj_set_style_bg_color(_btn_clear, lv_color_hex(0x27404F), 0);
    lv_obj_add_event_cb(_btn_clear, _onClearClicked, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(_btn_clear);
        lv_label_set_text(label, "清空");
        lv_obj_set_style_text_font(label, _font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_center(label);
    }

    _btn_cfg = lv_button_create(right);
    lv_obj_set_size(_btn_cfg, 128, 38);
    lv_obj_set_style_radius(_btn_cfg, 12, 0);
    lv_obj_set_style_bg_color(_btn_cfg, lv_color_hex(0x27404F), 0);
    lv_obj_add_event_cb(_btn_cfg, _onConfigClicked, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(_btn_cfg);
        /* LV_SYMBOL_SETTINGS 来自 montserrat（cbin 字库没有它，靠 fallback 链取到）。 */
        lv_label_set_text(label, LV_SYMBOL_SETTINGS " 设置");
        lv_obj_set_style_text_font(label, _font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_center(label);
    }

    /* ---------------- 消息列表 ---------------- */
    _list = lv_obj_create(_root);
    lv_obj_remove_style_all(_list);
    lv_obj_set_width(_list, LV_PCT(100));
    lv_obj_set_flex_grow(_list, 1);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_list, 4, 0);
    lv_obj_set_style_pad_row(_list, 12, 0);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_AUTO);

    /* ---------------- 输入行 ---------------- */
    lv_obj_t *row = lv_obj_create(_root);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 66);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    _ta = lv_textarea_create(row);
    lv_obj_set_flex_grow(_ta, 1);
    lv_obj_set_height(_ta, LV_PCT(100));
    lv_textarea_set_one_line(_ta, true);
    lv_textarea_set_placeholder_text(_ta, "输入指令，回车发送");
    lv_obj_set_style_text_font(_ta, _font, 0);
    lv_obj_set_style_text_color(_ta, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_bg_color(_ta, lv_color_hex(0x1A2127), 0);
    lv_obj_set_style_bg_opa(_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_ta, 0, 0);
    lv_obj_set_style_radius(_ta, 14, 0);
    lv_obj_set_style_pad_all(_ta, 12, 0);
    lv_obj_add_event_cb(_ta, _onInputClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_ta, _onReady, LV_EVENT_READY, this);

    _btn_mode = lv_button_create(row);
    lv_obj_set_size(_btn_mode, 84, LV_PCT(100));
    lv_obj_set_style_radius(_btn_mode, 14, 0);
    lv_obj_set_style_bg_color(_btn_mode, lv_color_hex(0x27404F), 0);
    lv_obj_add_event_cb(_btn_mode, _onModeClicked, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(_btn_mode);
        lv_label_set_text(label, "拼音");
        lv_obj_set_style_text_font(label, _font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_center(label);
    }

    lv_obj_t *send = lv_button_create(row);
    lv_obj_set_size(send, 118, LV_PCT(100));
    lv_obj_set_style_radius(send, 14, 0);
    lv_obj_set_style_bg_color(send, lv_color_hex(COLOR_USER), 0);
    lv_obj_add_event_cb(send, _onSendClicked, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(send);
        lv_label_set_text(label, "发送");
        lv_obj_set_style_text_font(label, _font, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }

    /* ---------------- 两个键盘（挂在 scr 上，脱离 _root 的 flex 流） ----------------
     * 为什么挂 scr：`lv_ime_pinyin_set_keyboard()` 会把候选栏 reparent 到键盘的父对象，
     * 并用 `lv_obj_align_to(..., LV_ALIGN_OUT_TOP_MID)` 把它贴在键盘上方。
     * 如果键盘是 flex 容器的子对象，flex 会接管候选栏的位置，贴不到键盘上面去。 */
    _kb_ime = lv_keyboard_create(scr);
    lv_obj_set_size(_kb_ime, screen_w, _kb_height);
    lv_obj_align(_kb_ime, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(_kb_ime, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(_kb_ime, _font, 0);
    lv_obj_add_flag(_kb_ime, LV_OBJ_FLAG_HIDDEN);
    _installSwitchKeyMap(_kb_ime);

    _kb_ascii = lv_keyboard_create(scr);
    lv_obj_set_size(_kb_ascii, screen_w, _kb_height);
    lv_obj_align(_kb_ascii, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(_kb_ascii, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(_kb_ascii, _font, 0);
    lv_obj_add_flag(_kb_ascii, LV_OBJ_FLAG_HIDDEN);
    _installSwitchKeyMap(_kb_ascii);

    /* 输入法：换掉 LVGL 内置那份 322 条、混着繁体的字典。 */
    _ime = lv_ime_pinyin_create(scr);
    lv_ime_pinyin_set_dict(_ime, const_cast<lv_pinyin_dict_t *>(hub_pinyin_dict));
    lv_ime_pinyin_set_keyboard(_ime, _kb_ime);
    lv_obj_add_flag(_ime, LV_OBJ_FLAG_HIDDEN);

    /* 候选栏是 `lv_ime_pinyin` 自己建的，没有出参交出来；`set_keyboard()` 又把它
     * reparent 到了键盘的父对象（= scr）⇒ 按"除我们两个键盘之外的 buttonmatrix"认出来。
     * （`lv_buttonmatrix_class` 是键盘的基类，所以两个键盘要先排除掉。） */
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t *child = lv_obj_get_child(scr, static_cast<int32_t>(i));
        if (child == nullptr || child == _kb_ime || child == _kb_ascii) {
            continue;
        }
        if (lv_obj_has_class(child, &lv_buttonmatrix_class)) {
            _cand_panel = child;
            break;
        }
    }
    _styleCandidatePanel();
    _updateBadge();
}

void Hub::_installSwitchKeyMap(lv_obj_t *kb)
{
    if (kb == nullptr) {
        return;
    }

    /* 小写和大写两套都换 —— 否则用户点一下大小写键，"√" 就又回来了。 */
    buildKeyMap(kb, LV_KEYBOARD_MODE_TEXT_UPPER, s_kb_map_upper);
    buildKeyMap(kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_kb_map_lower);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);

    /* 我们的回调注册在键盘自己那个之后 ⇒ 后跑，正好用来抹掉它刚插进去的字符。 */
    lv_obj_add_event_cb(kb, _onKeyboardValueChanged, LV_EVENT_VALUE_CHANGED, this);
}

void Hub::_onKeyboardValueChanged(lv_event_t *e)
{
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    lv_obj_t *kb = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    if (self == nullptr || kb == nullptr) {
        return;
    }

    const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *text = lv_buttonmatrix_get_button_text(kb, id);
    if (text == nullptr || strcmp(text, LV_SYMBOL_DOWN) != 0) {
        return;
    }

    /* 此刻 `lv_keyboard` 自己的 handler 已经跑过了，而且**不认识** LV_SYMBOL_DOWN
     *    ⇒ 它走了最后那个 `else`，把这个符号当成普通文本插进了 textarea。
     *    所以这里必须把它删掉（`lv_textarea_add_text` 会把光标推过去，
     *    `lv_textarea_del_char` 删的就是它）。
     *    —— 这是"改不了上游就擦掉它副作用"的写法，注释留着免得以后看不懂。
     *
     * 要删的是**键盘当前绑定的那个 textarea**，不能写死聊天输入框：
     *    连接设置浮层里键盘也会绑到 IP / 密码那些格子上（原来写死 `_ta`，
     *    在那里按 ↓ 就会去删聊天框里的一个字）。 */
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (ta != nullptr) {
        lv_textarea_delete_char(ta);   /* LVGL 9 里删"光标左边"那个字符 */
    }

    self->_hideKeyboards();
    ESP_UTILS_LOGD("收起键盘");
}

void Hub::_styleCandidatePanel(void)
{
    if (_cand_panel == nullptr) {
        ESP_LOGW(ESP_UTILS_LOG_TAG, "没找到候选栏，拼音候选可能不可读");
        return;
    }

    /* 为什么必须动它：`lv_ime_pinyin_constructor()` 把候选栏设成
     *   `bg_opa = LV_OPA_0`（全透明）、`radius 0`、`border 0`，
     * 然后 `set_keyboard()` 用 `align_to(..., LV_ALIGN_OUT_TOP_MID)` 把它贴在键盘正上方
     * ⇒ 默认形态是一排**悬空的字压在下面的内容上**。必须给它不透明的底。 */
    lv_obj_set_style_bg_color(_cand_panel, lv_color_hex(0x16202A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_cand_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(_cand_panel, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(_cand_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(_cand_panel, lv_color_hex(0x2A3946), LV_PART_MAIN);

    /* 每个候选按钮本身不要底色，选中态给一点点高亮 —— 否则一排方块很难看。 */
    lv_obj_set_style_bg_opa(_cand_panel, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_cand_panel, lv_color_hex(COLOR_USER), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(_cand_panel, LV_OPA_50, LV_PART_ITEMS | LV_STATE_CHECKED);

    /* 中文靠我们这份字库；◀ ▶ 翻页箭头靠 montserrat 的 fallback（见 _ensureFont）。 */
    lv_obj_set_style_text_font(_cand_panel, _font, LV_PART_MAIN);
    lv_obj_set_style_text_font(_cand_panel, _font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_cand_panel, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_color(_cand_panel, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);

    /* 高度改成固定的像素值（默认是 `LV_PCT(5)`），这样"给候选栏让出多少空间"
     * 是个确定数，不会因为横竖屏切换算出半像素。
     * `set_keyboard()` 里那次 `align_to` 只会在创建时调一次，之后不会再改尺寸。 */
    lv_obj_set_height(_cand_panel, _cand_height);

    ESP_UTILS_LOGI("候选栏已生效：高 %d px，全屏宽，不透明底", _cand_height);
}

/* ==========================================================================
 * 消息渲染
 * ========================================================================== */

lv_obj_t *Hub::_addBubble(chat::Role role, const std::string &text, bool error)
{
    _dropEmptyHint();

    lv_obj_t *row = lv_obj_create(_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          error ? LV_FLEX_ALIGN_START
                          : (role == chat::Role::User ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START),
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const uint32_t bg = error ? COLOR_ERROR
                        : (role == chat::Role::User ? COLOR_USER : COLOR_AGENT);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, LV_PCT(BUBBLE_WIDTH_PCT));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 18, 0);
    lv_obj_set_style_pad_all(bubble, 14, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(bubble);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, _font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(error ? 0xFFDAD6 : COLOR_TEXT), 0);
    lv_obj_set_style_text_line_space(label, 6, 0);
    lv_label_set_text(label, text.empty() ? "..." : text.c_str());

    /* 运行期裁剪：`historyLoad()` 里的裁剪只管**文件**，
     *    而发消息这条路是**无限追加**的 —— 用户实测"发了一堆，屏幕上不止 50 条"
     *    就是这个。卡顿的量取决于屏幕上挂着多少行，所以这一刀必须补。 */
    _trimBubbles();

    _scrollToBottom();
    return label;
}

void Hub::_trimBubbles(void)
{
    if (_list == nullptr) {
        return;
    }

    /* 只删 `_list` 的直接孩子（一行 = 一个气泡）。
     * `_empty_hint` 也是孩子，但它只在一条消息都没有时存在 —— 那时行数不可能超限。 */
    uint32_t trimmed = 0;
    while (lv_obj_get_child_count(_list) > static_cast<uint32_t>(chat::HISTORY_KEEP)) {
        lv_obj_t *oldest = lv_obj_get_child(_list, 0);
        if (oldest == nullptr) {
            break;
        }

        /* 万一这一行里正挂着"流式回复"的 label，必须先把映射摘掉 ——
         *    否则下一片回复会往已释放的内存里写字（拆解前必须先把指向它的指针撤掉）。
         *    实际上最老的那几行早就回完了，这条只是不指望"刚好不会发生"。 */
        for (auto it = _inflight.begin(); it != _inflight.end();) {
            lv_obj_t *p = it->second.label;
            while (p != nullptr && p != oldest) {
                p = lv_obj_get_parent(p);
            }
            if (p == oldest) {
                it = _inflight.erase(it);
            } else {
                ++it;
            }
        }

        lv_obj_del(oldest);
        ++trimmed;
    }

    if (trimmed > 0) {
        ESP_UTILS_LOGI("列表超过 %d 行 ⇒ 删掉最老的 %u 行", chat::HISTORY_KEEP,
                       static_cast<unsigned>(trimmed));
    }
}

void Hub::_scrollToBottom(void)
{
    if (_list == nullptr) {
        return;
    }
    /* 等布局算完再滚，否则滚的是"上一帧的末尾"（宽高是延迟生效的）。 */
    lv_obj_update_layout(_list);
    lv_obj_scroll_to_y(_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void Hub::_dropEmptyHint(void)
{
    if (_empty_hint == nullptr) {
        return;
    }
    lv_obj_del(_empty_hint);
    _empty_hint = nullptr;
}

void Hub::_renderHistory(void)
{
    const std::vector<Message> messages = historyLoad();

    if (messages.empty()) {
        _empty_hint = lv_label_create(_list);
        lv_label_set_text(_empty_hint,
                          _cfg.isConfigured()
                          ? "还没有消息。\n在下面输入指令，回车发送。\n\n"
                          "回复来自树莓派上的 Agent。关掉本页也会继续收。"
                          : "还没有消息。\n在下面输入指令，回车发送。\n\n"
                          "还没配置树莓派：点右上角 ⚙ 设置填 IP 与账号。");
        lv_obj_set_width(_empty_hint, LV_PCT(100));
        lv_label_set_long_mode(_empty_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(_empty_hint, _font, 0);
        lv_obj_set_style_text_color(_empty_hint, lv_color_hex(COLOR_HINT), 0);
        lv_obj_set_style_text_line_space(_empty_hint, 8, 0);
        lv_obj_set_style_pad_top(_empty_hint, 12, 0);
        return;
    }

    for (const auto &m : messages) {
        _addBubble(m.role, m.text, m.error);
    }
}

/* ==========================================================================
 * 清空本机记录
 *
 * 为什么需要它：列表里每条消息都是一个 `LV_LABEL_LONG_WRAP` 的 label，
 * LVGL 每帧都要把长文本重新排版 ⇒ 历史里堆了几段长回复之后，滑到那一段就像 PPT。
 * 用户点一下把本机记录清掉就恢复（Agent 那边的上下文**完全不受影响**）。
 * ========================================================================== */

void Hub::_clearHistory(void)
{
    if (_list == nullptr || _btn_clear == nullptr) {
        return;
    }

    /* `lv_tick_get()` 会回绕（约 49 天），所以比较用**有符号差**，别直接比大小。 */
    const bool armed = (_clear_armed_until != 0) &&
                       (static_cast<int32_t>(lv_tick_get() - _clear_armed_until) < 0);

    if (!armed) {
        _clear_armed_until = lv_tick_get() + CLEAR_CONFIRM_MS;
        lv_obj_t *label = lv_obj_get_child(_btn_clear, 0);
        if (label != nullptr) {
            lv_label_set_text(label, "确认清空");
        }
        lv_obj_set_style_bg_color(_btn_clear, lv_color_hex(COLOR_ERROR), 0);
        ESP_UTILS_LOGI("清空：等第二次确认（%u ms 内有效）", static_cast<unsigned>(CLEAR_CONFIRM_MS));
        return;
    }

    _clear_armed_until = 0;
    lv_obj_set_style_bg_color(_btn_clear, lv_color_hex(0x27404F), 0);
    {
        lv_obj_t *label = lv_obj_get_child(_btn_clear, 0);
        if (label != nullptr) {
            lv_label_set_text(label, "清空");
        }
    }

    /* ① 先撤掉所有"指向即将被删对象"的指针：
     *    - `_inflight` 里的 label 就长在 `_list` 里，留着它，下一片流式回复
     *      就会往已释放的内存写字；
     *    - `_empty_hint` 是 `_list` 的孩子，`lv_obj_clean()` 会连它一起删。
     * 清掉 `_inflight` 之后到达的回复不会被丢：`_drainReplies()` 走
     *    "it == end" 分支自己补一个新气泡（服务给的是累积全文，不会只显示半句）。 */
    _inflight.clear();
    _empty_hint = nullptr;

    const uint32_t removed = lv_obj_get_child_count(_list);
    lv_obj_clean(_list);                  /* 真删，不是隐藏 —— 隐藏的话内存照样占着 */
    const bool file_gone = chat::historyClear();

    /* ② 画回空态提示（`_renderHistory()` 在文件为空时就会画）。 */
    _renderHistory();

    ESP_UTILS_LOGI("已清空本机记录：界面 %u 个气泡；历史文件%s",
                   static_cast<unsigned>(removed),
                   file_gone ? "已删除" : "删除失败（下次开 app 会读回旧记录）");
}

void Hub::_updateClearButton(void)
{
    if (_btn_clear == nullptr || _clear_armed_until == 0) {
        return;
    }
    if (static_cast<int32_t>(lv_tick_get() - _clear_armed_until) < 0) {
        return;
    }

    /* 3 秒没点第二下 ⇒ 复原，防止"过一会儿手一滑"把记录清了。 */
    _clear_armed_until = 0;
    lv_obj_t *label = lv_obj_get_child(_btn_clear, 0);
    if (label != nullptr) {
        lv_label_set_text(label, "清空");
    }
    lv_obj_set_style_bg_color(_btn_clear, lv_color_hex(0x27404F), 0);
}

void Hub::_onClearClicked(lv_event_t *e)
{
    auto *self = static_cast<Hub *>(e->user_data);
    if (self != nullptr) {
        self->_clearHistory();
    }
}

/* ==========================================================================
 * 交互
 * ========================================================================== */

void Hub::_applyKeyboardSpace(void)
{
    if (_root == nullptr) {
        return;
    }
    const bool visible = (_kb_ime != nullptr && !lv_obj_has_flag(_kb_ime, LV_OBJ_FLAG_HIDDEN)) ||
                         (_kb_ascii != nullptr && !lv_obj_has_flag(_kb_ascii, LV_OBJ_FLAG_HIDDEN));
    const lv_coord_t screen_h = lv_display_get_vertical_resolution(nullptr);

    /* 键盘弹出来的时候，底部要让出「键盘 + 候选栏」两块。
     * 候选栏是贴在键盘正上方的，不让出这一段它就会压在输入行上（上一版就是这个毛病）。
     * 候选栏即使暂时隐藏也照样留空间 —— 否则"打出第一个字母"那一刻整个界面上跳一下。 */
    const lv_coord_t reserved = visible ? (_kb_height + _cand_height) : 0;
    lv_obj_set_height(_root, screen_h - reserved);
}

void Hub::_showKeyboard(bool ascii_mode)
{
    if (_kb_ime == nullptr || _kb_ascii == nullptr || _ta == nullptr) {
        return;
    }

    _ascii_mode = ascii_mode;

    if (ascii_mode) {
        lv_obj_add_flag(_kb_ime, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_kb_ascii, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(_kb_ascii, _ta);
    } else {
        lv_obj_add_flag(_kb_ascii, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_kb_ime, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(_kb_ime, _ta);
    }

    _applyKeyboardSpace();
    _scrollToBottom();
}

void Hub::_hideKeyboards(void)
{
    if (_kb_ime != nullptr) {
        lv_obj_add_flag(_kb_ime, LV_OBJ_FLAG_HIDDEN);
    }
    if (_kb_ascii != nullptr) {
        lv_obj_add_flag(_kb_ascii, LV_OBJ_FLAG_HIDDEN);
    }

    /* 候选栏必须**单独**藏（2026-11-16 用户报的 100% 复现：收起键盘后中文候选框留在屏上）。
     *    `lv_ime_pinyin_set_keyboard()` 会把候选栏 reparent 成键盘的**兄弟**
     *    （`lv_ime_pinyin.c:436`，因为候选栏要贴在键盘正上方、不能进键盘的布局），
     *    ⇒ 键盘加 `HIDDEN` 带不上它。
     *    重新打字时 IME 自己会 `lv_obj_remove_flag(..., HIDDEN)`（`lv_ime_pinyin.c:803`），
     *    所以这里只管藏、不用管还原。 */
    if (_cand_panel != nullptr) {
        lv_obj_add_flag(_cand_panel, LV_OBJ_FLAG_HIDDEN);
    }

    _applyKeyboardSpace();
}

/* ==========================================================================
 * 连接设置（顶栏入口）
 * ========================================================================== */

void Hub::_updateBadge(void)
{
    if (_badge == nullptr) {
        return;
    }

    std::string text;
    uint32_t color = COLOR_HINT;

    /* 状态全部来自常驻服务 —— app 只是把它画出来。 */
    if (!_cfg.isConfigured()) {
        text = "未配置 Pi";
    } else if (!services::HubService::certsPresent()) {
        text = "缺证书";
        color = COLOR_BAD;
    } else {
        switch (services::HubService::getInstance().state()) {
        case chat::MqttState::Connected:
            text = "● 已连接";
            color = COLOR_OK;
            break;
        case chat::MqttState::Connecting:
            text = "○ 连接中…";
            break;
        case chat::MqttState::Failed:
            text = "× 连接失败";
            color = COLOR_BAD;
            break;
        default:
            text = "未连接";
            break;
        }
    }

    /* 只在文字真的变了才动 LVGL —— 这个函数每 120 ms 被调一次。 */
    if (text == _badge_text) {
        return;
    }
    _badge_text = text;
    lv_label_set_text(_badge, text.c_str());
    lv_obj_set_style_text_color(_badge, lv_color_hex(color), 0);
}

void Hub::_openConfig(void)
{
    if (_config_overlay != nullptr) {
        return;
    }

    /* 关掉聊天键盘：否则它和浮层抢同一块屏幕，且 `_root` 的高度也被它压着。 */
    _hideKeyboards();

    _cfg = chat::configLoad();

    const lv_coord_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    const lv_coord_t screen_h = lv_display_get_vertical_resolution(nullptr);

    _config_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(_config_overlay);
    lv_obj_set_pos(_config_overlay, 0, 0);
    lv_obj_set_size(_config_overlay, screen_w, screen_h);
    lv_obj_set_style_bg_color(_config_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_config_overlay, LV_OPA_70, 0);
    lv_obj_remove_flag(_config_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* 卡片只占上半屏 —— 下半屏留给键盘（打开浮层就把 ASCII 键盘调起来）。 */
    const lv_coord_t card_h = screen_h - _kb_height - 24;

    lv_obj_t *card = lv_obj_create(_config_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, screen_w * CONFIG_CARD_WIDTH_PCT / 100, card_h);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141A20), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3946), 0);

    /* ---------------- 标题行 ---------------- */
    lv_obj_t *title_row = lv_obj_create(card);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, 36);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, "连接到树莓派");
    lv_obj_set_style_text_font(title, _font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);

    lv_obj_t *close_btn = lv_button_create(title_row);
    lv_obj_set_size(close_btn, 40, 36);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2A3946), 0);
    lv_obj_add_event_cb(close_btn, _onConfigClose, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(close_btn);
        lv_label_set_text(label, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_font(label, _font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_center(label);
    }

    /* ---------------- 四个字段 ---------------- */
    auto make_field = [&](const char *label_text, const char *placeholder, lv_obj_t **out) {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 46);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lab = lv_label_create(row);
        lv_label_set_text(lab, label_text);
        lv_obj_set_width(lab, CONFIG_LABEL_WIDTH);
        lv_obj_set_style_text_font(lab, _font, 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(COLOR_HINT), 0);

        lv_obj_t *ta = lv_textarea_create(row);
        lv_obj_set_flex_grow(ta, 1);
        lv_obj_set_height(ta, LV_PCT(100));
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, placeholder);
        lv_obj_set_style_text_font(ta, _font, 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_bg_color(ta, lv_color_hex(0x1A2127), 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_radius(ta, 10, 0);
        lv_obj_set_style_pad_all(ta, 8, 0);
        lv_obj_add_event_cb(ta, _onConfigFieldClicked, LV_EVENT_CLICKED, this);

        *out = ta;
    };

    make_field("树莓派 IP", "192.0.2.10", &_cfg_host);
    make_field("端口", "1883", &_cfg_port);
    lv_textarea_set_accepted_chars(_cfg_port, "0123456789");
    make_field("用户名", "MQTT 账号", &_cfg_user);
    make_field("密码", "MQTT 密码", &_cfg_pass);
    lv_textarea_set_password_mode(_cfg_pass, true);
    /* 默认的掩码字符是 U+2022（圆点），这份字库里没有 ⇒ 用 ASCII 星号。 */
    lv_textarea_set_password_bullet(_cfg_pass, "*");

    lv_textarea_set_text(_cfg_host, _cfg.host.c_str());
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(_cfg.port));
        lv_textarea_set_text(_cfg_port, buf);
    }
    lv_textarea_set_text(_cfg_user, _cfg.user.c_str());
    lv_textarea_set_text(_cfg_pass, _cfg.pass.c_str());

    /* ---------------- 加密开关 ---------------- */
    lv_obj_t *tls_row = lv_obj_create(card);
    lv_obj_remove_style_all(tls_row);
    lv_obj_set_width(tls_row, LV_PCT(100));
    lv_obj_set_height(tls_row, 40);
    lv_obj_set_flex_flow(tls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tls_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tls_row, 12, 0);
    lv_obj_remove_flag(tls_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tls_label = lv_label_create(tls_row);
    lv_label_set_text(tls_label, "加密（TLS）");
    lv_obj_set_width(tls_label, CONFIG_LABEL_WIDTH);
    lv_obj_set_style_text_font(tls_label, _font, 0);
    lv_obj_set_style_text_color(tls_label, lv_color_hex(COLOR_HINT), 0);

    _cfg_tls = lv_switch_create(tls_row);
    lv_obj_set_size(_cfg_tls, 64, 32);
    if (_cfg.tls) {
        lv_obj_add_state(_cfg_tls, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(_cfg_tls, _onTlsChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *tls_hint = lv_label_create(tls_row);
    lv_label_set_text(tls_hint, "1883 ⇄ 8883");
    lv_obj_set_style_text_font(tls_hint, _font, 0);
    lv_obj_set_style_text_color(tls_hint, lv_color_hex(COLOR_HINT), 0);

    /* ---------------- 提示 + 按钮 ---------------- */
    _cfg_msg = lv_label_create(card);
    lv_obj_set_width(_cfg_msg, LV_PCT(100));
    lv_label_set_long_mode(_cfg_msg, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_cfg_msg, "");
    lv_obj_set_style_text_font(_cfg_msg, _font, 0);
    lv_obj_set_style_text_color(_cfg_msg, lv_color_hex(COLOR_HINT), 0);

    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, 48);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *test_btn = lv_button_create(btn_row);
    lv_obj_set_size(test_btn, 176, 46);
    lv_obj_set_style_radius(test_btn, 12, 0);
    lv_obj_set_style_bg_color(test_btn, lv_color_hex(0x27404F), 0);
    lv_obj_add_event_cb(test_btn, _onConfigTest, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(test_btn);
        lv_label_set_text(label, "测试连接");
        lv_obj_set_style_text_font(label, _font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_center(label);
    }

    lv_obj_t *save_btn = lv_button_create(btn_row);
    lv_obj_set_size(save_btn, 140, 46);
    lv_obj_set_style_radius(save_btn, 12, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(COLOR_USER), 0);
    lv_obj_add_event_cb(save_btn, _onConfigSave, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *label = lv_label_create(save_btn);
        lv_label_set_text(label, "保存");
        lv_obj_set_style_text_font(label, _font, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }

    /* 直接给 IP 那一格焦点，用户打开就能敲。 */
    _showConfigKeyboard(_cfg_host);
}

void Hub::_closeConfig(void)
{
    /* 必须先解绑键盘再删浮层：`lv_keyboard_set_textarea()` 只把指针存进
     *    `keyboard->ta`，而 `lv_keyboard.c` **没有** LV_EVENT_DELETE 的清理钩子
     *    （239-257 行只做 add/remove FOCUSED 状态）。文本框被删后指针还留着，
     *    下次按键就是往已释放对象里写字 —— 典型的 use-after-free。
     *    必须在 `lv_obj_del()` **之前**调，否则这一步自己就会去碰野指针。 */
    if (_kb_ascii != nullptr) {
        lv_keyboard_set_textarea(_kb_ascii, nullptr);
    }

    if (_config_overlay != nullptr) {
        lv_obj_del(_config_overlay);
        _config_overlay = nullptr;
    }

    _cfg_host = nullptr;
    _cfg_port = nullptr;
    _cfg_user = nullptr;
    _cfg_pass = nullptr;
    _cfg_tls = nullptr;
    _cfg_msg = nullptr;

    if (_kb_ascii != nullptr) {
        lv_obj_add_flag(_kb_ascii, LV_OBJ_FLAG_HIDDEN);
    }
}

void Hub::_showConfigKeyboard(lv_obj_t *field)
{
    if (field == nullptr || _kb_ascii == nullptr) {
        return;
    }

    if (_kb_ime != nullptr) {
        lv_obj_add_flag(_kb_ime, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(_kb_ascii, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(_kb_ascii, field);

    /* 键盘必须压在遮罩之上，否则它会藏在那层半透明黑底后面，看得见但点不到。 */
    lv_obj_move_foreground(_kb_ascii);
}

void Hub::_showConfigMsg(const std::string &text, bool ok)
{
    if (_cfg_msg == nullptr) {
        return;
    }
    lv_label_set_text(_cfg_msg, text.c_str());
    lv_obj_set_style_text_color(_cfg_msg, lv_color_hex(ok ? COLOR_OK : COLOR_BAD), 0);
}

void Hub::_saveConfig(void)
{
    if (_cfg_host == nullptr || _cfg_port == nullptr) {
        return;
    }

    chat::BrokerConfig cfg;
    cfg.host = chat::internal::trim(lv_textarea_get_text(_cfg_host));
    cfg.user = chat::internal::trim(lv_textarea_get_text(_cfg_user));
    cfg.pass = std::string(lv_textarea_get_text(_cfg_pass));
    cfg.tls = (_cfg_tls != nullptr) && lv_obj_has_state(_cfg_tls, LV_STATE_CHECKED);

    const char *port_text = lv_textarea_get_text(_cfg_port);
    const int port = (port_text != nullptr) ? atoi(port_text) : 0;
    cfg.port = (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : 0;

    const std::string err = chat::configValidate(cfg);
    if (!err.empty()) {
        _showConfigMsg(err, false);
        return;
    }

    if (!chat::configSave(cfg)) {
        _showConfigMsg("保存失败（NVS 写不进去）", false);
        return;
    }

    _cfg = cfg;

    /* 连接参数变了 ⇒ 让常驻服务拆掉旧客户端、按新地址重建。
     * `restart()` 内部起短命任务去做 `esp_mqtt_client_stop()` —— 那个调用可能
     *    阻塞到秒级，绝不能放在 LVGL 任务里。 */
    const bool scheduled = services::HubService::getInstance().restart();

    _badge_text.clear();
    _updateBadge();

    if (scheduled) {
        _closeConfig();
    } else {
        _showConfigMsg("配置已保存，但重建连接排队失败 —— 请重启设备", false);
    }

    /* 密码永远不进日志。 */
    ESP_UTILS_LOGI("已保存树莓派配置：%s%s（%s）", cfg.summary().c_str(),
                   cfg.tls ? "（TLS）" : "", scheduled ? "正在重建连接" : "重建连接排队失败");
}

void Hub::_startTest(void)
{
    if (_cfg_host == nullptr || _cfg_port == nullptr) {
        return;
    }

    const std::string host = chat::internal::trim(lv_textarea_get_text(_cfg_host));
    const char *port_text = lv_textarea_get_text(_cfg_port);
    const int port = (port_text != nullptr) ? atoi(port_text) : 0;

    if (host.empty()) {
        _showConfigMsg("先填树莓派的 IP", false);
        return;
    }
    if (port <= 0 || port > 65535) {
        _showConfigMsg("端口要在 1~65535 之间", false);
        return;
    }

    bool busy = false;
    {
        std::lock_guard<std::mutex> lock(_test_mutex);
        busy = _test_inflight;
        if (!busy) {
            _test_host = host;
            _test_port = static_cast<uint16_t>(port);
            _test_done = false;
            _test_inflight = true;
        }
    }
    if (busy) {
        _showConfigMsg("上一次测试还没结束", false);
        return;
    }

    if (_cfg_msg != nullptr) {
        const std::string pending = "正在连接 " + host + ":" + std::to_string(port) + " …";
        lv_label_set_text(_cfg_msg, pending.c_str());
        lv_obj_set_style_text_color(_cfg_msg, lv_color_hex(COLOR_HINT), 0);
    }

    /* 独立任务：`getaddrinfo` + `connect` 可能在 DNS / 握手上耗到超时，
     * 放在 LVGL 任务里会让整个界面僵住。 */
    if (xTaskCreate(&Hub::_testTask, "hub_cfgtest", TEST_TASK_STACK, this, 4, nullptr) != pdPASS) {
        {
            std::lock_guard<std::mutex> lock(_test_mutex);
            _test_inflight = false;
        }
        _showConfigMsg("无法启动测试任务", false);
    }
}

void Hub::_drainTestResult(void)
{
    bool done = false;
    bool ok = false;
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(_test_mutex);
        if (_test_done) {
            done = true;
            ok = _test_ok;
            msg = _test_msg;
            _test_done = false;
            _test_inflight = false;
        }
    }

    if (done) {
        _showConfigMsg(msg, ok);
    }
}

void Hub::_testTask(void *arg)
{
    auto *self = static_cast<Hub *>(arg);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(self->_test_mutex);
        host = self->_test_host;
        port = self->_test_port;
    }

    bool ok = false;
    std::string msg;

    char service[8];
    snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    const int gai = getaddrinfo(host.c_str(), service, &hints, &res);
    if (gai != 0 || res == nullptr) {
        msg = "解析失败 —— IP 写对了吗？";
    } else {
        for (struct addrinfo *ai = res; ai != nullptr && !ok; ai = ai->ai_next) {
            const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) {
                continue;
            }

            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            bool connected = false;
            const int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc == 0) {
                connected = true;
            } else if (errno == EINPROGRESS) {
                fd_set write_set;
                FD_ZERO(&write_set);
                FD_SET(fd, &write_set);

                struct timeval tv;
                tv.tv_sec = TEST_CONNECT_TIMEOUT_MS / 1000;
                tv.tv_usec = (TEST_CONNECT_TIMEOUT_MS % 1000) * 1000;

                const int selected = select(fd + 1, nullptr, &write_set, nullptr, &tv);
                if (selected > 0) {
                    int sock_err = 0;
                    socklen_t len = sizeof(sock_err);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == 0 && sock_err == 0) {
                        connected = true;
                    } else {
                        msg = "端口拒绝连接 —— broker 起了吗？";
                    }
                } else if (selected == 0) {
                    msg = "连接超时 —— IP 不对或不在同一网段";
                } else {
                    msg = "连接失败";
                }
            } else {
                msg = "连接失败";
            }

            ::close(fd);   /* 必须带全局作用域：类作用域里 `close` 会命中 Hub::close() */
            ok = connected;
        }
        freeaddrinfo(res);

        if (ok) {
            msg = "连上了：网络和 broker 端口都通";
        } else if (msg.empty()) {
            msg = "连接失败";
        }
    }

    /* 量一下栈余量，别只写"我猜够用"。 */
    ESP_UTILS_LOGI("TCP 探活 %s:%u → %s（栈余量 %u B）", host.c_str(),
                   static_cast<unsigned>(port), msg.c_str(),
                   static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    {
        std::lock_guard<std::mutex> lock(self->_test_mutex);
        self->_test_done = true;
        self->_test_ok = ok;
        self->_test_msg = msg;
    }

    vTaskDelete(nullptr);
}

void Hub::_onConfigClicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_openConfig();
    }
}

void Hub::_onConfigClose(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_closeConfig();
    }
}

void Hub::_onConfigSave(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_saveConfig();
    }
}

void Hub::_onConfigTest(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_startTest();
    }
}

void Hub::_onConfigFieldClicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    auto *field = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (self != nullptr && field != nullptr) {
        self->_showConfigKeyboard(field);
    }
}

void Hub::_onTlsChanged(lv_event_t *e)
{
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (self == nullptr || sw == nullptr || self->_cfg_port == nullptr) {
        return;
    }

    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    const char *cur_text = lv_textarea_get_text(self->_cfg_port);
    const int cur = (cur_text != nullptr) ? atoi(cur_text) : 0;

    /* 只在"当前值还是另一档默认端口"时才跟着换 —— 不覆盖用户手填的端口。 */
    uint16_t want = 0;
    if (cur == 0) {
        want = on ? chat::BROKER_PORT_TLS : chat::BROKER_PORT_PLAIN;
    } else if (on && cur == chat::BROKER_PORT_PLAIN) {
        want = chat::BROKER_PORT_TLS;
    } else if (!on && cur == chat::BROKER_PORT_TLS) {
        want = chat::BROKER_PORT_PLAIN;
    }

    if (want != 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(want));
        lv_textarea_set_text(self->_cfg_port, buf);
    }
}

void Hub::_submit(void)
{
    if (_ta == nullptr) {
        return;
    }

    const char *raw = lv_textarea_get_text(_ta);
    if (raw == nullptr || raw[0] == '\0') {
        return;
    }

    const std::string text = raw;
    lv_textarea_set_text(_ta, "");
    _hideKeyboards();

    _addBubble(chat::Role::User, text, false);
    historyAppend(Message{chat::Role::User, text, false});

    std::string id;
    if (!services::HubService::getInstance().send(text, id)) {
        /* 契约：断连期间不排队。服务在上线前直接返回 false。 */
        std::string reason;
        if (!_cfg.isConfigured()) {
            reason = "未发送：还没配置树莓派 —— 点右上角 ⚙ 设置填 IP 与账号。";
        } else if (!services::HubService::certsPresent()) {
            reason = "未发送：固件里没有证书，连不上树莓派。";
        } else if (!services::HubService::getInstance().isConnected()) {
            reason = "未发送：现在没连上树莓派（离线时不排队，请稍后重发）。";
        } else {
            reason = "未发送：传输层拒绝了这条消息。";
        }
        _addBubble(chat::Role::Agent, reason, true);
        return;
    }

    /* 先占一个"..."气泡，回执到了再逐片填字 —— 这样用户一点发送就有反馈。 */
    Inflight inflight;
    inflight.label = _addBubble(chat::Role::Agent, "...", false);
    _inflight[id] = inflight;
    ESP_UTILS_LOGI("已发送 id=%s", id.c_str());
}

void Hub::_drainReplies(void)
{
    std::vector<services::ChatReply> replies;
    {
        std::lock_guard<std::mutex> lock(_pending_mutex);
        replies.swap(_pending);
    }
    if (replies.empty()) {
        return;
    }

    for (const auto &reply : replies) {
        auto it = _inflight.find(reply.id);
        if (it == _inflight.end()) {
            /* 服务是常驻的 ⇒ 可能收到"上一次进 app 时发出、这次才有回执"的帧
             * （比如发完就退出 app、回复在后台流完又进来）。
             * 用户那条消息已经在历史里了，这里补一个 Agent 气泡接上就行。
             * `reply.text` 是**累积全文**，所以不会只显示半句。 */
            Inflight inflight;
            inflight.label = _addBubble(chat::Role::Agent, "...", false);
            it = _inflight.emplace(reply.id, inflight).first;
        }
        Inflight &inflight = it->second;

        if (!reply.error.empty()) {
            if (inflight.label != nullptr) {
                lv_label_set_text(inflight.label, ("错误：" + reply.error).c_str());
                lv_obj_set_style_text_color(inflight.label, lv_color_hex(0xFFDAD6), 0);
            }
            if (reply.final) {
                historyAppend(Message{chat::Role::Agent, "错误：" + reply.error, true});
                _inflight.erase(it);
            }
            _scrollToBottom();
            continue;
        }

        /* 快照语义：服务给的是"到目前为止的全文"，不是这一片的增量。 */
        inflight.text = reply.text;
        if (inflight.label != nullptr) {
            lv_label_set_text(inflight.label, inflight.text.c_str());
        }

        if (reply.final) {
            historyAppend(Message{chat::Role::Agent, inflight.text, false});
            _inflight.erase(it);
        }
        _scrollToBottom();
    }
}

void Hub::_drainUnread(void)
{
    const std::vector<services::ChatReply> unread =
        services::HubService::getInstance().takeUnread();
    if (unread.empty()) {
        return;
    }

    for (const auto &reply : unread) {
        if (!reply.error.empty()) {
            const std::string text = "错误：" + reply.error;
            _addBubble(chat::Role::Agent, text, true);
            historyAppend(Message{chat::Role::Agent, text, true});
        } else if (!reply.text.empty()) {
            _addBubble(chat::Role::Agent, reply.text, false);
            historyAppend(Message{chat::Role::Agent, reply.text, false});
        }
    }
    _scrollToBottom();
    ESP_UTILS_LOGI("补上 app 未打开期间完成的 %u 条回复", static_cast<unsigned>(unread.size()));
}

/* ==========================================================================
 * 事件回调
 * ========================================================================== */

void Hub::_onPoll(lv_timer_t *t)
{
    auto *self = static_cast<Hub *>(t->user_data);
    if (self != nullptr) {
        self->_drainReplies();
        self->_drainTestResult();
        self->_updateBadge();
        self->_updateClearButton();
    }
}

void Hub::_onSendClicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_submit();
    }
}

void Hub::_onReady(lv_event_t *e)
{
    /* 单行 textarea 上按回车 = 发送 */
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_submit();
    }
}

void Hub::_onInputClicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_showKeyboard(self->_ascii_mode);
    }
}

void Hub::_onModeClicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *self = static_cast<Hub *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }

    self->_ascii_mode = !self->_ascii_mode;

    if (self->_btn_mode != nullptr) {
        uint32_t count = lv_obj_get_child_count(self->_btn_mode);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t *child = lv_obj_get_child(self->_btn_mode, static_cast<int32_t>(i));
            if (child != nullptr && lv_obj_has_class(child, &lv_label_class)) {
                lv_label_set_text(child, self->_ascii_mode ? "英文" : "拼音");
            }
        }
    }

    self->_showKeyboard(self->_ascii_mode);
}

} // namespace esp_brookesia::apps
