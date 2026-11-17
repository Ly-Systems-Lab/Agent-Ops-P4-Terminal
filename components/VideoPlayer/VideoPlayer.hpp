#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/ppa.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"
#include "sdkconfig.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "avi_player.h"
#include "lvgl.h"
/* 硬件 JPEG 解码器（P4 上有；软件版 esp_new_jpeg 留着做对照，见 .cpp 里的说明）。
 * 为什么从 esp_jpeg_dec.h 换过来：软件解码 1280x720 实测 174 ms/帧（≈5.1 fps），
 * 而 P4 自带硬件 JPEG 解码器，代价从"每帧 150+ ms"降到"几 ms"。 */
#include "driver/jpeg_decode.h"

namespace esp_brookesia::apps
{

    class VideoPlayer : public systems::phone::App
    {
    public:
        static VideoPlayer *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
        ~VideoPlayer();

    protected:
        VideoPlayer(bool use_status_bar, bool use_navigation_bar);
        bool run(void) override;
        bool back(void) override;
        bool close(void) override;
        bool init(void) override;
        bool deinit(void) override;
        bool pause(void) override;
        bool resume(void) override;

    private:
        static VideoPlayer *_instance;

        static constexpr int SWIPE_EXIT_THRESHOLD = 180;
        /* 双击窗口。播放中的触摸是 30 ms 一拍地轮询，人手一次点击大约 50~150 ms
         * ⇒ 两拍之内基本一定分别采得到"按下/抬起"，350 ms 对人手是宽松值。 */
        static constexpr int64_t DOUBLE_TAP_WINDOW_US = 350 * 1000;

        lv_display_t *display = nullptr;
        esp_lcd_panel_handle_t display_panel = nullptr;
        ppa_client_handle_t ppa_srm_handle = nullptr;
        esp_lcd_touch_handle_t touch_handle = nullptr;
        void *lcd_buffers[CONFIG_BSP_LCD_DPI_BUFFER_NUMS] = {};
        int current_buf_idx = 0;
        uint8_t *jpeg_output_buffer = nullptr;
        size_t jpeg_output_buffer_size = 0;
        size_t data_cache_line_size = 64;
        uint16_t last_video_width = 0;
        uint16_t last_video_height = 0;
        /* ---- 视频画面旋转----
         * 三档：0 / 90 / 270。（90° 与 270° 的**几何完全相同**，只差内容朝向 ⇒
         * 做成循环、用户按到"看上去对"为止，就不必赌哪个方向"应该"对。）
         * `last_video_rot` 必须参与"构图变了没"的判据 —— 否则换了角度不改源尺寸，
         * letterbox 的黑边不会重清，上一档的残影会留在帧缓冲里。
         * 跨任务访问：`rot_requests` 由**播放任务**写（pollTouch 那条路），
         * 由 **videoCallback**（avi_player 的 30 Hz 帧任务）读 ⇒ 只让读的那一方消费它，
         * 这样 `video_rot_idx` 始终只有一个写者。 */
        int video_rot_idx = 0;              /* 0 ⇒ 0°、1 ⇒ 90°、2 ⇒ 270°（跨次播放保留） */
        volatile int rot_requests = 0;      /* 双击一次 +1；videoCallback 消费后清零 */
        int last_video_rot = -1;            /* 上一帧**实际用到**的角度（-1 = 本轮还没画过） */
        int64_t last_tap_us = 0;            /* 上次"单击抬手"的时刻（双击窗口用） */
        /* **构图变化时清缓冲**：三代做法，前两代都是坑，别走回去。
         *
         * ① 「只清四条黑边」⇒ 逐行小 memset 造成跨缓存行的残缺写 ⇒ 黑边频闪。
         * ② 修「接下来 3 帧，每帧清即将写入的那个缓冲的**整屏**」⇒ 竖屏片
         *    "跳大"后旋转会留下**快速闪烁的彩色细条**（用户 2026-10-19 截图）。
         *    根因不在"逐帧"，而在**缓存**：PPA 搬运前会对自己输出窗口做 M2C 失效
         *    （`ppa_srm.c:278-282`，整行宽 × 新块高）⇒ 刚 memset 进 CPU cache 的 0
         *    被**直接丢弃**；blit 末尾的 C2M 又只写回"仍然脏"的行 ⇒ 旧像素永久留存。
         *    （2 MB memset 的尾部还留在 cache 里，所以残条只出现在**底部** —— 与实测吻合。）
         * ③ 修「构图变化时**当场清全部缓冲** 6 MB」⇒ 正确，但 **39 ms/次** 的停顿
         *    让双击旋转出现明显卡顿（用户 2026-10-20）。
         *
         * **现在的做法**：每个缓冲各记自己上一版画的矩形 `buf_last_blk[]`，
         *    画之前只清「**旧块 − 新块**」那一圈残留（上/下两条 + 中段左右两侧），
         *    并**立刻 C2M 写回 PSRAM**。实测 0°→90° 9 ms/缓冲、90°→0° 1.3 ms/缓冲，
         *    且不会漏（每个缓冲自己记账，连续快速切换也正确）。见 .cpp 的
         *    `clearLeftoverRect()` / `clearRectInBuf()`。**别再整屏清。** */

        /* ---- 自动识别「左右补黑边的竖构图」----
         * 竖屏短视频经 PC 转码时为了不裁切，被缩进 800×450 并补了左右黑边
         * ⇒ 整幅 800×450 再放进 800×1280 的竖屏后，真正有内容的只剩中间
         * 252×450 那一条，用户看到的就是"太小"。
         *
         * 做法：在**每部片子的第一帧**就量黑边（判据很严，见 .cpp 里的 measurePillarbox），
         * 可信就立刻采纳，然后把 PPA 的输入块裁到内容区，再走原来的等比 letterbox ⇒
         * 内容变成约 716×1280（左右各剩约 42 px 黑边），无裁切、无变形。
         *
         * **只在 `vid_rot == 0` 时使用**：用户明确"这些竖屏视频旋转后的
         *    播放效果已足够"，90°/270° 那条路的构图保持原样。
         * 检测状态**每部片子重置**（连播时同一个 display bypass 会换片）；
         *    但 `buf_last_blk[]` **故意不跟着重置** —— 新片第一帧与上一部
         *    构图不同时要靠它把上一部留下的残边清掉。 */
        static constexpr int PB_MAX_ATTEMPTS = 90;   /* ≈3 s @30fps，之后不再试 */
        /* 有黑边：**第一帧可信就采纳**。判据本身已经很严（两侧黑边 ≥2%、宽度对称、
         * 中间必须是竖构图），单帧误判的概率极低；而"等 5 帧再采纳"会让用户先看到
         * 0.17 秒的小画面再"跳大"（用户实测反馈）⇒ 提前采纳后，第一帧显示的就是最终构图。
         * 无黑边仍然要 10 帧 —— 它不改变画面，只决定"以后不再检测"，宁可多等。 */
        static constexpr int PB_FOUND_AGREE  = 1;
        static constexpr int PB_NONE_AGREE   = 10;
        int  pb_left = 0;           /* 采纳的内容区左边界（源坐标，0 = 不裁） */
        int  pb_width = 0;          /* 采纳的内容区宽度（0 = 不裁） */
        int  pb_cand_left = 0;      /* 候选值（最近一帧） */
        int  pb_cand_w = 0;
        int  pb_agree = 0;          /* 候选值连续一致的帧数 */
        int  pb_attempts = 0;       /* 已尝试帧数 */
        bool pb_done = false;       /* 已定案（有黑边 / 无黑边都算） */
        uint16_t last_video_crop_x = 0;
        uint16_t last_video_crop_w = 0;
        /* 每个帧缓冲**当前画着的那块画面**（输出坐标）。
         * 构图变化时只需要清「旧块 − 新块」这一圈残留，不必整屏清 6 MB ——
         * 整屏清实测 39 ms/次（2026-10-20 探针），双击旋转时就是一次可感知的卡顿。
         * `w == 0` 表示这个缓冲里是"空白（全黑）"，也就是没有残留要清。 */
        struct BlkRect {
            uint16_t x, y, w, h;
        };
        BlkRect buf_last_blk[CONFIG_BSP_LCD_DPI_BUFFER_NUMS] = {};
        static void clearRectInBuf(uint8_t *fb, int x, int y, int w, int h);
        static void clearLeftoverRect(void *fb, const BlkRect &prev, const BlkRect &cur);

        bool loop_playback = true;
        bool is_playing = false;
        bool is_paused = false;
        bool sd_mounted = false;
        bool audio_ready = false;
        /* 已经应用到 codec 上的音频格式，用来**跳过无意义的重配**。
         *
         * 为什么必须判重：`bsp_extra_codec_set_fs()` 内部是
         * `esp_codec_dev_close()` → `esp_codec_dev_open()` → 解除静音，
         * 而且底层会把 I2S TX 通道**停掉再重建**（实测日志每次都有
         * `i2s_channel_disable` → 重建 I2S → `Open codec device OK`）。
         * 偏偏 `avi_player` **每开一个片都会回调一次**音频格式
         * （avi_player.c:142-148，在 HEADER 状态里），哪怕格式一个字都没变
         * ⇒ 不判重的话，**每换一次片就把整条音频通路拆了重建一遍**。
         * 用户听到的那道接缝"咔哒"就是这个 —— 而且**没有音轨的片子也照样响**，
         * 这正说明它与音频数据无关（实测）。 */
        bool audio_fs_valid = false;
        uint32_t audio_fs_rate = 0;
        uint32_t audio_fs_bits = 0;
        uint32_t audio_fs_ch = 0;
        /* ====================================================================
         * 音频投递：**独立任务 + 环形槽位**
         *
         * 为什么必须独立出去（根因，不是优化）：
         *   · `avi_player` 只在它自己那个 **30 Hz 帧定时器**上回调 `audio_cb`
         *     （avi_player.c:150-152 起定时器、:259-264 每个 tick 调一次），
         *     而回调里的 `bsp_extra_i2s_write` **是阻塞的**，并且和
         *     **阻塞 11~12 ms 的 PPA** 挤在**同一个任务**里（.cpp:1513
         *     `PPA_TRANS_MODE_BLOCKING`）。
         *   · 每个节拍要喂的音频量 = 176400 B/s × 33.33 ms = **5880 B**，
         *     而 I2S TX DMA 全部只有 6×240×4 = **5760 B = 32.65 ms**
         *     ⇒ **稳态余量约为 0**。任何一个节拍迟到，音频就永久落在后面
         *     （"慢出来的量再也还不上"）；DMA 见底时 `auto_clear`
         *     （= `auto_clear_after_cb`，i2s_common.h:69 明写"没数据就自动发零"）
         *     **直接补零** ⇒ 短促的取样丢失。
         *   · 这种"补零缺口"叠在**响**的内容上是个"咔"，叠在静音上还是静音
         *     ⇒ 用户听到的就是"**人说话时有电流音**"。
         *
         * 关键对照（用户实测）：**同一份 PCM** 在**音乐播放器**里完全干净 ——
         * 而 `bsp_extra_player_init()` 里 `.write_fn` **也是 `bsp_extra_i2s_write`**，
         * codec / I2S / 音量（两边都是 main.cpp 的 80）**全部相同**
         * ⇒ **唯一的自变量就是"喂的节奏"**。
         *
         * 做法：`audio_cb` 只把数据**拷进一个空闲槽就立刻返回**（不阻塞视频任务），
         * 由下面的任务从"就绪槽"取出来 `portMAX_DELAY` 地喂 I2S。
         * ⇒ DMA 一直有货可发；视频任务也不再被阻塞 46 ms。
         * 槽位必须**拷贝**：`avi_player` 的 `pbuffer` 是音频和视频**共用**的
         * （avi_player.c:167/182/197 都指向同一个 `avi_player.data.pbuffer`），
         * 不拷贝就返回的话，下一个 chunk 会当场把它覆盖掉。
         * ================================================================== */
        static constexpr size_t AUDIO_SLOT_BYTES = 32 * 1024;  /* 单 chunk 上限（实测最大 16384）*/
        static constexpr int    AUDIO_SLOTS      = 4;           /* 4 × 32 KB = 128 KB PSRAM */
        struct AudioSlot {
            uint8_t *buf = nullptr;
            size_t   len = 0;
        };
        QueueHandle_t audio_free_q  = nullptr;  /* 装空闲槽的 `uint8_t*` */
        QueueHandle_t audio_ready_q = nullptr;  /* 装填好的 AudioSlot */
        TaskHandle_t  audio_task_handle = nullptr;
        volatile bool audio_task_stop = false;

        bool startAudioTask();
        bool stopAudioTask(TickType_t timeout);
        static void audioTask(void *arg);

        /* 音频通路探针。判据要能对上**可核对的物理常数**：
         *   · `bytes` 应当 == AVI 里音频 chunk 的字节总和（01.avi = 17,637,320）
         *     ⇒ 少一个字节就是丢过音频；
         *   · `dropped` 非零 = 生产者抢不到空闲槽（这条路自己就成了新的欠载源）；
         *   · `idle` = **"上一次 I2S 写返回" → "这一次写开始"** 的空转时长。
         *     它由 `audioTask` 逐次直测（见那个函数里的 `prev_end`），
         *     统计放在 `c9c::audioIdle()` 里，**不占 .hpp 的成员**。
         * ====================================================================
         * **旧版这里记的是 `waits`（50 ms 超时次数），那是一个坏尺子**：
         *   · 它的粒度是 **50 ms** —— 比被测对象（整条 DMA = **32,650 µs**）**还粗**：
         *     一次超时必然见底（信息量为 0），而"只空了 8 ms"这种更常见、
         *     真正在啃余量的空窗，**压根不会超时，一个也记不到**。
         *   · 它还把"网格上歇着""换片之间"的空等**一起算进来**
         *     （实测换片那一瞬能到 1391）⇒ 被稀释成没意义。
         *   · 我当年据此写下"`waits` 不是欠载指标" —— **那句话只在"空闲期"成立**；
         *     在**播放中**它确实是欠载指标，只是粒度太粗，等于没量。
         * ⇒ 改成**直测 `idle`**，判据不需要任何模型：
         *     **`idle > 32,650 µs` ⇒ 无论空之前管子里剩多少，必定见底 ⇒ 必定响一声。**
         * **别在音频任务里"每写一次打一行 UART"**：一行 ~80 B @2 Mbaud =
         *     **400 µs 阻塞**，44 次/秒 ⇒ 每秒白占 17 ms ⇒ **探针自己会造出欠载**
         *     。所以只在 `idle ≥ 20 ms`（已经吃掉 61% 余量）时才逐条打。
         * ==================================================================== */
        uint32_t ap_chunks  = 0;
        uint32_t ap_dropped = 0;
        int64_t  ap_bytes   = 0;
        int      ap_slot_peak = 0;      /* 取走一槽时队列里还剩几槽（抖动余量） */

        /* **换片防爆音**：新片子开头 N 帧做一个从 0 起坡的淡入。
         * 为什么需要：换片时 `avi_player_play_from_file` 要开文件 + 解头
         * （**实测 221~273 ms**），这段时间**一个字节音频都没有**
         * ⇒ I2S 的 `auto_clear` 直接补零 ⇒ 新片子的音频**从数字静音里突变开始**
         * ⇒ 波形上是一道直上直下的台阶 ⇒ 一声"咔"。
         * （音乐播放器那条路本来就有 mute/unmute 防爆音，视频这边什么都没有。）
         * 只改**我们自己拷进槽里的那一份**，不碰 avi_player 的共享 pbuffer。
         * 只在 16 bit 时启用（非 16 bit 直接跳过，不做会算错的坡）。 */
        static constexpr int FADE_IN_FRAMES = 1024;   /* ≈23 ms @44.1 kHz：够抹掉台阶，又短到听不出是淡入 */
        int fade_in_left  = 0;          /* 还要坡多少帧（>0 才做） */
        int fade_smp_pf   = 2;          /* 一帧几个 16-bit 样点（2ch ⇒ 2） */

        /* **收尾淡出**（这才是"咔"的真凶）。
         * 用户的两条观察把它一次钉死（不是我推的）：
         *   · **无声视频 → 有声视频：不响** ⇒ "从静音起跳"**根本不咔**（上升沿无罪）；
         *   · **播放中途滑回预览界面也响** ⇒ 咔 = **正在播的内容被硬切到 0**。
         * ⇒ 要停的时候，先往管子里补一段**从"最后写出去的那个样点值"平滑降到 0** 的斜坡。
         * **起点必须接得上**（斜坡第一帧 = 上次写出的值），否则"内容→斜坡"之间又生一道台阶。
         * 只铺一次/片；铺两处：① 播放中滑走 ② 自然播完。 */
        static constexpr int FADE_OUT_FRAMES = 1024;  /* ≈23 ms；顺带盖住换片那 ~30 ms 空档 */
        int16_t last_l = 0;             /* 最后一次写出去的左声道样点（淡出的起点） */
        int16_t last_r = 0;             /* 右声道（单声道时与 last_l 相同） */
        bool fade_out_pushed = false;   /* 每片只铺一次 */
        /* **专用淡出缓冲**：不能和环形池抢槽。
         * 证据（用户实测的不对称）：同一个 `pushFadeOut()`，**播放中滑走 ⇒ 修好了**、
         * **换片 ⇒ 没修好**。两条路的差别只在"那一刻槽位宽不宽裕" ——
         * 滑走时生产者早停了；换片时生产者刚倾泻完最后几块、槽位正被占着
         * ⇒ 我第一版那两条"抢不到就静默放弃"的判据，**在换片那条路上正好全中**。
         * ⇒ 给它一块**只属于它自己**的内存（不还回队列），并给就绪队列一个短等待。 */
        uint8_t *fade_buf = nullptr;
        uint32_t fade_ok = 0;           /* 本片成功铺出几次淡出 */
        uint32_t fade_fail = 0;         /* 本片**没铺成**几次（>0 = 这一声必然还在） */
        void pushFadeOut();

        /* **avi_player 的读缓冲**（它一个字节都不多用，见 .cpp 里那段推导）。
         * 硬约束只有一条：**必须 ≥ 片子里最大的那个 chunk**（`read_frame` 装不下就报错）。
         * 历史：早期全卡最大 chunk = 51,213 B ⇒ 定 **128 KB**；后来的高清素材
         * （720×1280 / q6）把单帧推到 **168.2 KB（gq08）/ 192.0 KB（gq09）** ⇒ 128 KB **装不下**
         * （`read_frame` 返回 0 ⇒ 直接停播），所以**升到 256 KB**。
         * 代价只有换片时多读 128 KB（≈110 µs/KB ⇒ **+13 ms**，仍远小于 I2S DMA 98 ms）。
         * **别再往上调到 MB 级** —— 那会让"换片"多读几 MB，
         * 把换片断口从 30 ms 拉到 250 ms ⇒ 管子流空 ⇒ 一声"咔"。 */
        static constexpr size_t AVI_READ_BUFFER_BYTES = 256 * 1024;
        size_t max_chunk_seen = 0;      /* 本片见过最大的 chunk（盯上面的余量） */

        /* **两个任务栈的大小（实测定，不是拍的）。**
         * 用 `uxTaskGetStackHighWaterMark` 跨「2 次开关 app / 4 部片子 / 6 次旋转」实测：
         *   · `avi_play_task`（我们的控制任务，**内部** RAM）峰值 **2,472 B**
         *     —— 最深一处是 `showSelectGrid()` 的 2,320 B，比播放期还深；
         *   · `avi_player` 自己的任务（**PSRAM**）峰值 **2,548 B**。
         * ⇒ 都取 **8192**（3.2~3.3 倍余量）。
         * 原来两个都是 32768（13 倍冗余）。而 `avi_play_task` 要"一整块连续 32 KB
         *    **内部** RAM"正是当年 `Create playback task failed` 的真因 ——
         *    缺内存只是表象，**要得太多才是**。
         * 改这两个数之前重新量一遍；探针怎么插见 .cpp 里 `xTaskCreatePinnedToCore`
         *    上方那段注释（调试结束后探针已删）。 */
        static constexpr size_t AVI_PLAY_TASK_STACK   = 8192;
        static constexpr size_t AVI_PLAYER_TASK_STACK = 8192;

        bool dummy_enabled = false;
        bool lvgl_paused = false;
        bool touch_active = false;
        uint16_t touch_start_x = 0;
        uint16_t touch_start_y = 0;

        /* 没有 `close_wait_task` —— "playTask 停好了没"现在**轮询 `play_task_handle`**，
         * 不再用 task notification（LVGL 任务上挂着别人的通知，见 .cpp 的说明）。 */
        TaskHandle_t play_task_handle = nullptr;

        avi_player_handle_t avi_player_handle;
        jpeg_decoder_handle_t jpeg_handle = nullptr;

        char **avi_file_list = nullptr;
        int avi_file_count = 0;

        lv_obj_t *status_label = nullptr;

        /* ====================================================================
         * 视频选择网格（预览页，"类似相册预览"）
         *
         * 为什么必须在 run() 里做、而不是在播放任务里：**"显示旁路"只在
         * playTask 存活期间有效**（它接管 LCD 帧缓冲、直接往里 blit）。
         * 网格是普通 LVGL 界面，必须在旁路之外 ⇒ 两者互斥、来回切。
         *
         * 网格上的 LVGL 对象**只能由 LVGL 任务碰**（或持有 display_lock）。
         * 唯一例外是缩略图填充 —— 那是 lv_timer 回调，本来就在 LVGL 任务里。
         * ================================================================== */
        struct VideoCell {
            lv_obj_t *img  = nullptr;   /* 缩略图对象；没补好之前 HIDDEN */
            lv_obj_t *note = nullptr;   /* 占位/失败文字（红字）；与 img 二者只显示一个 */
            uint8_t  *px   = nullptr;   /* RGB565 像素（PSRAM）；nullptr = 还没补 */
            lv_image_dsc_t dsc = {};
            const char *fail = nullptr; /* 非空 ⇒ 本格放弃，原因就是这个（静态字串，别 free） */
        };
        VideoCell *cells = nullptr;     /* 长度 = avi_file_count */
        lv_obj_t *grid_obj = nullptr;
        int thumb_next = 0;             /* 下一个要补的格子（一格一拍） */
        int play_from_index = 0;        /* 点格子时记下"从哪个开始播" */
        bool back_to_grid = false;      /* 播放中**滑了一下**屏幕 ⇒ 回网格（单击不再触发） */

        /* 网格 ↔ 播放 的交接**只走这个变量** —— LVGL 回调里只写它，
         * 真正的切换（删对象 / 起播放任务）由 playTask 的轮询循环做。
         * 为什么：事件回调是在 LVGL 遍历对象树的过程中跑的，在里面把整棵树删掉
         * 是经典的用后释放。
         *
         * **网格上没有"退出 app"这个变量** —— 退出完全交给框架自带的下方手势条
         * （见 showSelectGrid() 的说明：同一次滑动被两边处理正是"黑屏有背光"的来源）。 */
        int grid_pick = -1;             /* ≥0 ⇒ 用户点了这个序号 */
        int64_t grid_built_us = 0;      /* 网格建好的时刻：头 500 ms 忽略点击（防"开 app 那一下"） */
        /* `avi_player_play_stop()` 只是置事件位，真正的 `fclose()` 在任务里。收尾若再 stop
         * 一次、又和循环里那次挨太近，任务会把 STOP 消费两遍 ⇒ **第二次 fclose** ⇒ 重启。
         * 这个布尔就是记账："这一轮已经请求过 stop 了，别再请求"。详见 playTask 收尾的注释。 */
        bool avi_stop_requested = false;

        void showSelectGrid();
        /* 返回 false = 显示锁被占（框架正在打断我们）⇒ **这次没删**，
         * 网格留给下一次 showSelectGrid()/close() 收。见 .cpp 里的说明。 */
        bool destroySelectGrid();
        void freeThumbnails();
        void fillNextThumb();
        void applyThumbToCell(int i);
        static void onCellClicked(lv_event_t *e);

        bool startPlaybackTask();
        bool stopPlaybackTask(TickType_t timeout);
        esp_err_t getAviFileList(const char *dir_path);
        esp_err_t initDisplayBypass();
        void deinitDisplayBypass();
        esp_err_t initJpegDecoder();
        void deinitJpegDecoder();
        esp_err_t ensureJpegOutputBuffer(size_t out_len);
        void releaseJpegOutputBuffer();
        /* 触摸轮询。判据是**手指抬起时**总位移有没有超过阈值。
         * `DoubleTap` = 双击 = 切视频画面旋转；`Tap`（单击）**不再有任何动作** ——
         * 因为一次双击里必然含一次单击，单击若还留着"回网格"，双击会先把人踢回网格。
         * `Swipe` 仍然是"回上一级（预览网格）"，与改动前**完全一致**（这块一个字没动）。 */
        enum class TouchAction { None, Tap, Swipe, DoubleTap };
        TouchAction pollTouch();
        bool touch_moved = false;       /* 本次触摸里位移是否已越过阈值 */
        uint16_t touch_last_x = 0;      /* 最后一次看到的位置：抬起时用它结算位移
                                         * （快速一划可能在下一次轮询前就抬手了） */
        uint16_t touch_last_y = 0;

        static void playTask(void *arg);
        static void videoCallback(frame_data_t *data, void *arg);
        static void audioCallback(frame_data_t *data, void *arg);
        static void audioSetClockCallback(uint32_t rate, uint32_t bits_cfg, uint32_t ch, void *arg);
        static void playEndCallback(void *arg);
    };

} // namespace esp_brookesia::apps
