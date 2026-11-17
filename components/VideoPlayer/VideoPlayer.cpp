/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "VideoPlayer.hpp"
#include "lvgl.h"
#include "esp_brookesia.hpp"

#include "bsp/touch.h"
#include "esp_lib_utils.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
/* 清帧缓冲**必须**在紧接着的 PPA 之前把数据真正写回 PSRAM（见 videoCallback
 * 里那次 `esp_cache_msync` 的说明）。没有这一句，PPA 的 pre-transfer M2C 失效会把
 * 我们缓存在 CPU cache 里的清零数据**直接丢弃**，旧像素就留在屏幕上。 */
#include "esp_cache.h"
/* 音频喂送任务用队列在"视频任务"和"音频任务"之间传槽位。
 * `freertos/queue.h` 不是必然被别的头带进来（TaskHandle_t 来自 task.h），显式包含。 */
#include "freertos/queue.h"
/* 这两个是**真在用**的（不是探针遗留）：`_scanAviDir` 靠 errno/dirent/stat
 * 扫目录并判断是不是常规文件（见 is_regular_avi），删了会编不过。
 * （曾在此留过一句"探针专用、调试完一起删"的注释，那是错的，已订正。） */
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>      /* 预览网格取首帧要用 fopen/fseek/fread */
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:VideoPlayer"

LV_IMG_DECLARE(img_app_vedioplayer);

#define DISPLAY_WIDTH  BSP_LCD_H_RES
#define DISPLAY_HEIGHT BSP_LCD_V_RES
#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB565
#define VIDEO_JPEG_OUT_FORMAT   JPEG_DECODE_OUT_FORMAT_RGB565
#define VIDEO_PPA_COLOR_MODE    PPA_SRM_COLOR_MODE_RGB565
#define VIDEO_BYTES_PER_PIXEL   2
#elif CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
#define VIDEO_JPEG_OUT_FORMAT   JPEG_DECODE_OUT_FORMAT_RGB888
#define VIDEO_PPA_COLOR_MODE    PPA_SRM_COLOR_MODE_RGB888
#define VIDEO_BYTES_PER_PIXEL   3
#else
#error "Unsupported BSP LCD color format"
#endif

/* 硬件解码器的 R/B 顺序。
 * 选 BGR 的**依据不是猜**：IDF 官方示例 `examples/peripherals/jpeg/jpeg_decode`
 * （jpeg_decode_main.c:107）和原厂 `10_mp4_player`（app_stream_adapter.c:155-157）
 * **两处独立用法都写 BGR**。驱动的枚举注释把 BGR/RGB 说成"小端/大端"，字面上看不出
 * 跟 RGB565 的位序怎么对应，所以只能靠这两个官方用法定。
 * 如果上屏后**红蓝互换**，把这里改成 JPEG_DEC_RGB_ELEMENT_ORDER_RGB 即可（一处改动）。 */
#define VIDEO_JPEG_RGB_ORDER    JPEG_DEC_RGB_ELEMENT_ORDER_BGR

/* 视频画面旋转的三档。取**三档**而不是两档，是因为 90° 与 270° 的几何完全相同
 * （都是"横竖互换后按同一套 letterbox 算"），差的只是内容朝向 —— 做成循环让用户
 * 按到"看上去对"为止，就不必去赌哪个方向"应该"对。 */
static const int kVideoRotDeg[3] = {0, 90, 270};

namespace esp_brookesia::apps
{


    /* 算出硬件解码器**实际输出**的行宽/行高（= MCU 对齐后的尺寸）。
     *
     * 依据 esp_driver_jpeg/jpeg_parse_marker.c:188-198：
     *   mcux = hi[0]*8, mcuy = vi[0]*8; process = ALIGN_UP(origin, mcu)
     * 而采样方式与 MCU 的对应是 YUV420→16x16、YUV422→16x8、YUV444/灰度→8x8。
     * jpeg_decoder_get_info() 给回的是 **SOF 里的原始尺寸**（jpeg_decode.c:140-150 填 origin），
     * 所以对齐要我们自己补。
     * 这个值同时是"缓冲要多大"和"PPA 的 pic_w/pic_h 填多少"，两边必须一致。 */
    static void mcuAlignedSize(const jpeg_decode_picture_info_t &info,
                               uint32_t *out_w, uint32_t *out_h)
    {
        uint32_t ax = 8;
        uint32_t ay = 8;
        switch (info.sample_method) {
        case JPEG_DOWN_SAMPLING_YUV420:
            ax = 16;
            ay = 16;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
            ax = 16;
            ay = 8;
            break;
        default:            /* YUV444 / 灰度 */
            break;
        }
        *out_w = (info.width + ax - 1) / ax * ax;
        *out_h = (info.height + ay - 1) / ay * ay;
    }


    /* ========================================================================
     * 量「左右补黑边」的竖屏片
     *
     * 背景：`mp4_to_avi.py` 为了"不裁切"，会把 1080×1920 的竖片等比缩进
     * 800×450 再补左右黑边 ⇒ 真正有内容的只有中间一条。播放器原来把整幅
     * 800×450 等比放进 800×1280 的竖屏，内容只剩 252×450（用户："太小了"）。
     *
     * 判据全部来自实测，不是猜的（2026-10-19，采样 0s/3s/10s/20s 完全一致）：
     *   · `01/02/03.avi`：左黑 272、内容 252、右黑 276 —— 整段稳定；
     *   · `04.avi` / `t2_*.avi`：左右黑边宽度都是 **0**（满幅）⇒ 一刀切开，
     *     横屏视频那条路天然不受影响。
     *
     * 只认「**左右**都有黑边 + 中间是**竖构图**」。上下黑边（宽银幕片）和
     * 横构图一律不动 —— 宁可不裁，也不要替用户放大一部正常的电影。
     * ====================================================================== */
    enum class PillarboxClass {
        Unknown = 0,   /* 整帧全黑 / 不可判（淡入、黑场）⇒ 下一帧再试，不表态 */
        None,          /* 没有对称的左右黑边 ⇒ 满幅片，保持原构图 */
        Found,         /* 检出左右黑边 + 中间竖构图 ⇒ 可以裁到内容区 */
    };

    struct PillarboxMeasure {
        PillarboxClass cls = PillarboxClass::Unknown;
        uint32_t left = 0;
        uint32_t width = 0;
    };

    static PillarboxMeasure measurePillarbox(const uint8_t *buf, uint32_t stride_px,
                                             uint32_t w, uint32_t h)
    {
        PillarboxMeasure m;
        if (!buf || w < 64 || h < 16) {
            return m;
        }

        /* 逐列统计"黑"的比例，行方向均匀取 64 行。
         * 只在前几帧跑，单帧 64×800 ≈ 5.1 万次读 —— 开销可以忽略。 */
        const uint32_t row_step = (h >= 64u) ? (h / 64u) : 1u;
        int first = -1;
        int last = -1;
        for (uint32_t x = 0; x < w; x++) {
            int sampled = 0;
            int black = 0;
            for (uint32_t y = row_step / 2u; y < h; y += row_step) {
                const uint16_t p =
                    reinterpret_cast<const uint16_t *>(buf)[y * stride_px + x];
                const uint32_t r5 = (p >> 11) & 0x1Fu;
                const uint32_t g6 = (p >> 5) & 0x3Fu;
                const uint32_t b5 = p & 0x1Fu;
                sampled++;
                /* 近黑：RGB565 的 (5,6,5) 档位 ≤ (2,4,2)，约等于 8-bit 的 16/16/16。
                 * 实测这三部片子的补边是**纯 0**，阈值留了压缩噪声的余量。 */
                if (r5 <= 2u && g6 <= 4u && b5 <= 2u) {
                    black++;
                }
            }
            if (sampled == 0) {
                continue;
            }
            /* 一列里 ≥98% 的采样行都是黑的才算黑边（避免暗场景误伤） */
            const bool is_bar = (black * 100 >= sampled * 98);
            if (!is_bar) {
                if (first < 0) {
                    first = static_cast<int>(x);
                }
                last = static_cast<int>(x);
            }
        }

        if (first < 0 || last < 0) {
            return m;   /* 整帧都是黑：判不了，下一帧再试 */
        }

        const uint32_t left_bar = static_cast<uint32_t>(first);
        const uint32_t right_bar = w - 1u - static_cast<uint32_t>(last);
        const uint32_t content_w = static_cast<uint32_t>(last - first + 1);
        const uint32_t min_bar = w / 50u;   /* 黑边至少占 2% 宽 */
        const uint32_t asym = (left_bar > right_bar) ? (left_bar - right_bar)
                             : (right_bar - left_bar);

        /* 任一条不满足 ⇒ None（保持原构图）：
         *   ① 左右都有黑边；② 两侧宽度接近（真补边天然对称）；
         *   ③ 内容明显比整幅窄；④ 内容是竖构图；⑤ 内容不是细条（防误判）。 */
        if (left_bar < min_bar || right_bar < min_bar ||
                asym > min_bar + 8u ||
                content_w >= w - min_bar ||
                static_cast<uint64_t>(content_w) * 100u >= static_cast<uint64_t>(h) * 95u ||
                content_w < h / 4u) {
            m.cls = PillarboxClass::None;
            return m;
        }

        m.cls = PillarboxClass::Found;
        m.left = static_cast<uint32_t>(first);
        m.width = content_w;
        return m;
    }

    static inline int intDiff(int a, int b)
    {
        return (a > b) ? (a - b) : (b - a);
    }

    /* ========================================================================
     * **只清残留**，不整屏清
     *
     * 为什么不能整屏清：实测 3 × 2 MB 的 memset + 写回要 **39 ms**（2026-10-20
     * 探针 `Full clear 3 frame buffers: 39.x ms`），而 0° 竖屏本身每帧已经要 36 ms
     * ⇒ 每双击旋转一次就多一次可感知的卡顿（用户："卡顿 + 电流杂音"）。
     *
     * 正确的最小集合：每个帧缓冲里只要清「**上一版画面块 − 新画面块**」那一圈
     * （上/下两条 + 中间段左右两侧，最多 4 块）。块内会被这次 PPA 重画；
     * 旧块之外本来就是黑的（不变量，由进播放时那次全清建立）。
     * ====================================================================== */
    void VideoPlayer::clearRectInBuf(uint8_t *fb, int x, int y, int w, int h)
    {
        if (!fb || w <= 0 || h <= 0) {
            return;
        }
        const size_t stride = static_cast<size_t>(DISPLAY_WIDTH) * VIDEO_BYTES_PER_PIXEL;
        const size_t row_bytes = static_cast<size_t>(w) * VIDEO_BYTES_PER_PIXEL;
        uint8_t *row = fb + (static_cast<size_t>(y) * DISPLAY_WIDTH + x) * VIDEO_BYTES_PER_PIXEL;
        for (int i = 0; i < h; i++) {
            memset(row, 0, row_bytes);
            row += stride;
        }
        /* 必须**在紧跟的 PPA 之前**把清零数据写回 PSRAM：PPA 搬运前会对自己
         * 的输出窗口做一次 M2C 失效（`ppa_srm.c:278-282`，窗口是整行宽 × 新块高）。
         * 若清零数据还只躺在 CPU cache 的脏行里，那次失效会把它们**直接丢弃**
         * ⇒ 块外的旧像素原样留下，而 blit 末尾的 C2M 只写回"仍然脏"的行
         * ⇒ 旧像素永久留存（实测症状：竖屏跳大后旋转，屏幕下方左右两条彩色
         * 细条快速闪烁，复现率 100%）。 */
        esp_cache_msync(fb + (static_cast<size_t>(y) * DISPLAY_WIDTH + x) *
                        VIDEO_BYTES_PER_PIXEL,
                        static_cast<size_t>(h - 1) * stride + row_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    void VideoPlayer::clearLeftoverRect(void *fbv, const BlkRect &prev, const BlkRect &cur)
    {
        if (!fbv || prev.w == 0 || prev.h == 0) {
            return;
        }
        uint8_t *const fb = static_cast<uint8_t *>(fbv);

        const int px0 = prev.x, py0 = prev.y;
        const int px1 = prev.x + prev.w, py1 = prev.y + prev.h;
        const int cx0 = cur.x, cy0 = cur.y;
        const int cx1 = cur.x + cur.w, cy1 = cur.y + cur.h;

        /* 上：旧块中在新块上沿之上的部分 */
        if (py0 < cy0) {
            const int bottom = (py1 < cy0) ? py1 : cy0;
            if (bottom > py0) {
                clearRectInBuf(fb, px0, py0, px1 - px0, bottom - py0);
            }
        }
        /* 下：旧块中在新块下沿之下的部分 */
        if (py1 > cy1) {
            const int top = (py0 > cy1) ? py0 : cy1;
            if (py1 > top) {
                clearRectInBuf(fb, px0, top, px1 - px0, py1 - top);
            }
        }
        /* 中段（与新块垂直重叠的行）里，旧块伸到新块左边/右边的部分 */
        const int oy0 = (py0 > cy0) ? py0 : cy0;
        const int oy1 = (py1 < cy1) ? py1 : cy1;
        if (oy0 < oy1) {
            if (px0 < cx0) {
                const int right = (px1 < cx0) ? px1 : cx0;
                if (right > px0) {
                    clearRectInBuf(fb, px0, oy0, right - px0, oy1 - oy0);
                }
            }
            if (px1 > cx1) {
                const int left = (px0 > cx1) ? px0 : cx1;
                if (px1 > left) {
                    clearRectInBuf(fb, left, oy0, px1 - left, oy1 - oy0);
                }
            }
        }
    }

    /* ========================================================================
     * 视频选择网格（预览页）—— 几何常量 + 取首帧的小工具
     *
     * 缩略图**必须按最终显示尺寸预生成**：LVGL 里只有 scale==256（正好 1:1）
     * 才走 memcpy 快路径，只要不等于 256 就掉进逐像素重采样。
     * 所以"图片框尺寸 == 缩略图尺寸"，而且按源画面比例算 ⇒ 永远不触发缩放。
     * 本工程素材是 800×450（16:9），224×126 也是 16:9。
     * ====================================================================== */
    static constexpr int GRID_THUMB_MAX_W = 224;
    static constexpr int GRID_THUMB_MAX_H = 126;
    static constexpr int GRID_CELL_W = GRID_THUMB_MAX_W + 12;   /* 236 */
    static constexpr int GRID_CELL_H = GRID_THUMB_MAX_H + 52;   /* 178：图 + 文件名 */
    /* 上限：每格缩略图 224×126×2 ≈ 55 KB 常驻 PSRAM，64 格 ≈ 3.5 MB。
     * 超了必须**在屏幕上红字写明**，绝不静默截断。 */
    static constexpr int GRID_MAX_CELLS = 64;

    /* 从 AVI 里取出**第一个视频帧**（一张 JPEG）。
     * 只扫到第一个 `00dc`/`00db` 块就停 —— 网格只要一张图，不做完整头部解析。
     * 返回的缓冲由调用方 free()；失败返回 false。 */
    static bool _firstVideoFrame(const char *path, uint8_t **out_buf, size_t *out_len)
    {
        *out_buf = nullptr;
        *out_len = 0;

        FILE *f = fopen(path, "rb");
        if (!f) {
            return false;
        }

        /* 先在前 16 KB 里找 'movi'（本工程素材的头都 < 10 KB） */
        static uint8_t head[16384];
        const size_t n = fread(head, 1, sizeof(head), f);
        long movi = -1;
        for (size_t i = 0; i + 4 <= n; i++) {
            if (memcmp(head + i, "movi", 4) == 0) {
                movi = static_cast<long>(i) + 4;
                break;
            }
        }
        if (movi < 0 || fseek(f, movi, SEEK_SET) != 0) {
            fclose(f);
            return false;
        }

        /* 一个块一个块往前跳，直到第一个视频块。
         * guard 是防坏文件把我们卡在这儿的（正常第 1 个块就是视频）。 */
        for (int guard = 0; guard < 8192; guard++) {
            uint8_t h[8];
            if (fread(h, 1, sizeof(h), f) != sizeof(h)) {
                break;
            }
            const uint32_t sz = static_cast<uint32_t>(h[4]) |
                                (static_cast<uint32_t>(h[5]) << 8) |
                                (static_cast<uint32_t>(h[6]) << 16) |
                                (static_cast<uint32_t>(h[7]) << 24);
            if (memcmp(h, "00dc", 4) == 0 || memcmp(h, "00db", 4) == 0) {
                if (sz == 0 || sz > 4u * 1024u * 1024u) {
                    break;      /* 明显不合理 ⇒ 别照样 malloc 一个天文数字 */
                }
                uint8_t *buf = static_cast<uint8_t *>(malloc(sz));
                if (!buf || fread(buf, 1, sz, f) != sz) {
                    free(buf);
                    break;
                }
                fclose(f);
                *out_buf = buf;
                *out_len = sz;
                return true;
            }
            if (fseek(f, static_cast<long>(sz + (sz & 1)), SEEK_CUR) != 0) {
                break;
            }
        }

        fclose(f);
        return false;
    }

    VideoPlayer *VideoPlayer::_instance = nullptr;

    VideoPlayer *VideoPlayer::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new VideoPlayer(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    VideoPlayer::VideoPlayer(bool use_status_bar, bool use_navigation_bar) : App("VideoPlayer", &img_app_vedioplayer, true, use_status_bar, use_navigation_bar), sd_mounted(false)
    {
        display = nullptr;
        display_panel = nullptr;
        current_buf_idx = 0;
        ppa_srm_handle = nullptr;
        touch_handle = nullptr;
        jpeg_handle = nullptr;
        jpeg_output_buffer = nullptr;
        jpeg_output_buffer_size = 0;
        avi_player_handle = nullptr;
        play_task_handle = nullptr;
        is_playing = false;
        is_paused = false;
        loop_playback = false;
        dummy_enabled = false;
        lvgl_paused = false;
        touch_active = false;
        avi_file_list = nullptr;
        avi_file_count = 0;
        last_video_width = 0;
        last_video_height = 0;
    }

    VideoPlayer::~VideoPlayer()
    {
        close();
    }

    bool VideoPlayer::run(void)
    {
        ESP_UTILS_LOGD("Run");
        is_paused = false;
        if (!sd_mounted) {
            esp_err_t mount_ret = bsp_sdcard_mount();
            sd_mounted = mount_ret == ESP_OK || mount_ret == ESP_ERR_INVALID_STATE;
            if (!sd_mounted) {
                ESP_LOGE(
                    ESP_UTILS_LOG_TAG,
                    "Mount %s failed: %s",
                    BSP_SD_MOUNT_POINT,
                    esp_err_to_name(mount_ret)
                );
            }
        }
        bsp_extra_display_lock(-1);
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);
        status_label = lv_label_create(lv_scr_act());
        lv_label_set_text(status_label, sd_mounted ? "loading files..." : "sd error");
        lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(status_label, DISPLAY_WIDTH);
        lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
        bsp_extra_display_unlock();

        if (!sd_mounted)
        {
            return true;
        }

        if (getAviFileList(BSP_SD_MOUNT_POINT "/avi") != ESP_OK || avi_file_count == 0)
        {
            bsp_extra_display_lock(-1);
            lv_label_set_text(status_label, "avi error");
            bsp_extra_display_unlock();
            return true;
        }

        bsp_extra_display_lock(-1);
        lv_obj_del(status_label);
        status_label = nullptr;
        bsp_extra_display_unlock();

        return startPlaybackTask();
    }

    bool VideoPlayer::back(void)
    {
        ESP_UTILS_LOGD("Back");
        // If the app needs to exit, call notifyCoreClosed() to notify the core to close the app
        ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
        return true;
    }

    bool VideoPlayer::close()
    {
        ESP_UTILS_LOGD("Close");
        if (!stopPlaybackTask(pdMS_TO_TICKS(2000))) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Playback task did not stop before close");
            return false;
        }

        deinitJpegDecoder();
        releaseJpegOutputBuffer();
        deinitDisplayBypass();

        bsp_extra_display_lock(-1);
        if (status_label)
        {
            lv_obj_del(status_label);
            status_label = nullptr;
        }
        /* 网格收尾：**只断引用、不 del**。
         * 网格对象是 `lv_scr_act()` 的孩子 ⇒ 框架关 app 销毁默认屏幕时会把它们一起删掉，
         * 我们再去 `lv_obj_del` 就是二次释放（实测就是这么崩的）。
         * （正常情况下 playTask 退出时已经 destroySelectGrid() 删过自己的那棵树了。） */
        grid_obj = nullptr;
        for (int i = 0; cells && i < avi_file_count; i++)
        {
            if (cells[i].img)
            {
                /* 先**断开图片对缩略图内存的引用**再 free —— 否则框架稍后销毁屏幕时
                 * LVGL 可能还会去画它，而那块 PSRAM 已经还给系统了（用后释放）。 */
                lv_image_set_src(cells[i].img, nullptr);
            }
            cells[i].img = nullptr;
            cells[i].note = nullptr;
        }
        freeThumbnails();
        bsp_extra_display_unlock();

        if (avi_file_list)
        {
            for (int i = 0; i < avi_file_count; i++)
            {
                free(avi_file_list[i]);
            }
            free(avi_file_list);
            avi_file_list = nullptr;
            avi_file_count = 0;
        }

        return true;
    }

    bool VideoPlayer::init()
    {
        ESP_UTILS_LOGD("Init");
        // Mount lazily in run() so inserting a card after boot is supported and
        // application installation does not initialize SDMMC unnecessarily.
        return true;
    }

    bool VideoPlayer::deinit()
    {
        ESP_UTILS_LOGD("Deinit");
        return true;
    }

    bool VideoPlayer::pause()
    {
        ESP_UTILS_LOGD("Pause");
        /* 框架切走 app 时会调这里（下方手势条上滑 = HOME = `processAppPause`），
         * 而且**它是在 LVGL 任务上调的** —— 所以 `stopPlaybackTask()` 里那个
         * "别用 task notification"的改动对这条路径是**必需**的，不是优化。
         *
         * 超时给到 4 s 是余量：退出流程里要 `deinitDisplayBypass()`（把显示还给 LVGL）
         * + `avi_player_deinit()`（它自己最多等 1 s）。实测正常路径 ~100~300 ms。 */
        if (!stopPlaybackTask(pdMS_TO_TICKS(4000)))
        {
            /* 任务没停下来 ⇒ 它可能还要用那块输出缓冲，**不能**放（宁可泄漏不能悬空）。 */
            return false;
        }


        /* **进后台也要把解码这层放干净**（原来只有 `close()` 会放）。
         *
         * 播放任务退出时只 `deinitDisplayBypass()`（3 块帧缓冲，6 MB 由它放掉），
         * 而 **JPEG 输出缓冲（720×1280×2 = 1,843,200 B）和硬件解码引擎只在 `close()` 里释放**
         * ⇒ "上滑进最近任务"之后它一直挂着。相册在后台/前台的另一半就是这么坏的：
         * 用户实测 VideoPlayer 挂后台再进相册 ⇒ 缩略图 `decode failed`、
         * 大图放大卡在模糊期（解明细图要 ~4.5 MB，分配不到）。
         *
         * 安全性：`stopPlaybackTask()` 返回 true 意味着 `avi_player` 自己的任务已经
         * `deinit` 完（帧回调不可能再进来），而 `initJpegDecoder()` / `ensureJpegOutputBuffer()`
         * 都在帧回调里**按需重建**（两者都判空/判容量），所以 `resume()` 后自然恢复。 */
        deinitJpegDecoder();
        releaseJpegOutputBuffer();
        return true;
    }

    bool VideoPlayer::resume()
    {
        ESP_UTILS_LOGD("Resume");
        if (!sd_mounted || !avi_file_list || avi_file_count == 0) {
            return true;
        }
        return startPlaybackTask();
    }

    bool VideoPlayer::startPlaybackTask()
    {
        if (play_task_handle) {
            return true;
        }
        if (!avi_file_list || avi_file_count == 0) {
            return false;
        }

        loop_playback = true;
        is_paused = false;
        avi_stop_requested = false;

        /* **栈大小按实测定，不是拍的**。
         * 先纠正一句旧注释：AVI 读盘 / JPEG 解码 / PPA 搬运**不在这个任务里** ——
         *    它们在 `avi_player` 自己的任务上（下方 cfg 的 `stack_in_psram = true`）。
         *    本任务只做：建/拆网格、补缩略图、开关显示旁路、播放期 30 ms 一次的触摸轮询。
         * 探针（`uxTaskGetStackHighWaterMark`，调试结束后已删）跨**两轮**实测：
         *   首轮 2 次开关 app / 4 部片子（800×450 与 720×1280）/ 6 次旋转（含 1.5 s 连按 5 下）；
         *   复测 6 部片子 / 13 次旋转（6 s 内连按）。
         *   两轮读数**完全一致**：本任务峰值 **2,472 B**（最深一处是 `showSelectGrid()`
         *   的 2,320 B），`avi_player` 峰值 **2,548 B**。
         * ⇒ 8192 = **3.3 倍余量**。原来那个 32768 是 13 倍冗余，而"要一块连续 32 KB
         *   内部 RAM"正是当年 `Create playback task failed` 的真因。
         * **改这个数之前重新量一遍**：在 playTask 里插
         *    `ESP_LOGI(TAG, "used %u", AVI_PLAY_TASK_STACK - uxTaskGetStackHighWaterMark(nullptr))`
         *    并在各阶段读一次（只在"创新低"时才打，否则日志会刷屏）。 */
        BaseType_t ret = xTaskCreatePinnedToCore(
            playTask,
            "avi_play_task",
            AVI_PLAY_TASK_STACK,
            this,
            7,
            &play_task_handle,
            0
        );
        if (ret != pdPASS) {
            loop_playback = false;
            play_task_handle = nullptr;
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Create playback task failed");
            return false;
        }

        return true;
    }

    bool VideoPlayer::stopPlaybackTask(TickType_t timeout)
    {
        loop_playback = false;
        is_paused = false;

        if (!play_task_handle) {
            return true;
        }
        if (xTaskGetCurrentTaskHandle() == play_task_handle) {
            return false;
        }

        /* 同一轮里 stop 只能请求一次（理由见 playTask 收尾那段注释）：
         * playTask 可能刚刚也为"滑动回网格"请求过 stop，两条路再各请求一次
         * ⇒ 任务把 STOP 消费两遍 ⇒ 第二次 `fclose()` 已释放的 FILE ⇒ 重启。 */
        if (avi_player_handle && !avi_stop_requested) {
            avi_stop_requested = true;
            avi_player_play_stop(avi_player_handle);
        }

        /* **不要用 `ulTaskNotifyTake` 等 playTask 退出**（原厂是那么写的，我们改掉）。
         *
         * 它有一个只在"从 LVGL 任务调用"时才现形的缺陷：
         * **`esp_lv_adapter` 的显示桥也用 task notification 通知 LVGL 任务**
         * （`display/bridge/v9/lvgl_bridge_v9.c:1919/2084` 的
         *  `vTaskNotifyGiveFromISR(notify_task, …)`，而 `notify_task` 就是桥在
         *  `xTaskGetCurrentTaskHandle()` 里存下的 LVGL 任务句柄）。
         * 那些通知只要没人取走，就会**一直挂**在 LVGL 任务的 notification 值上
         * ⇒ 我们这里 `ulTaskNotifyTake` **立刻**返回非 0 ⇒ 代码以为"playTask 已经退出"，
         *    于是即使 `play_task_handle` 还非空也直接判超时。
         *
         * 而框架调 `pause()`（手势回调）和 `close()`（`back()` 之后）时**正是在 LVGL 任务上**
         * ⇒ 这个假超时必然发生。实测代价：`avi_player_play_stop` 到
         * `Timed out waiting for playback task` **只隔 14 ms**（配置的超时是 2000/4000 ms）。
         * ⇒ `pause()` 假失败 ⇒ 框架 `processNavigationEvent(HOME)` 在
         *    `processAppPause()` 处 `goto end`，**整个"切回桌面"被中止**
         *    （`processDisplayScreenChange(MAIN)` 根本没执行）；
         *    可我们的 playTask 其实已经停了、显示也已经交还给 LVGL 了、LVGL 又没有失效区域要重画
         * ⇒ **黑屏有背光，要再滑一次才回桌面**（用户 2026-10-16 报的）。
         *    第二次成功是因为那时 `play_task_handle` 已经是 nullptr，函数在开头就返回 true。
         *
         * ⇒ 改成**轮询 `play_task_handle`**：playTask 在**拆完显示之后**才把它置空，
         *   所以"它变 nullptr"这个判据同时保证了"显示已经交还"。
         *   这个变量只由我们自己写，不会被任何第三方通知干扰。 */
        const TickType_t step = pdMS_TO_TICKS(10);
        const TickType_t wait_t0 = xTaskGetTickCount();
        for (TickType_t waited = 0; play_task_handle && waited < timeout; waited += step) {
            vTaskDelay(step);
        }
        /* 探针：把"等了多久"变成数字。正常应当是几十~几百 ms；
         * 如果这里出现 4000 这种数，就说明 playTask 被谁卡住了（不是"假超时"）。 */
        ESP_LOGI(ESP_UTILS_LOG_TAG, "playTask stop wait = %u ms",
                 (unsigned)((xTaskGetTickCount() - wait_t0) * portTICK_PERIOD_MS));

        if (play_task_handle) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Timed out waiting for playback task");
            return false;
        }

        return true;
    }

    esp_err_t VideoPlayer::getAviFileList(const char *dir_path)
    {
        DIR *dir = opendir(dir_path);
        if (!dir) {
            ESP_LOGW(
                ESP_UTILS_LOG_TAG,
                "Open AVI directory failed: %s (errno=%d)",
                dir_path,
                errno
            );
            return ESP_ERR_NOT_FOUND;
        }

        auto is_regular_avi = [dir_path](const struct dirent *entry) {
            if (!entry) {
                return false;
            }
            const char *ext = strrchr(entry->d_name, '.');
            if (!ext || strcasecmp(ext, ".avi") != 0) {
                return false;
            }
            if (entry->d_type == DT_REG) {
                return true;
            }
            if (entry->d_type != DT_UNKNOWN) {
                return false;
            }

            char path[384] = {};
            int written = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
            if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) {
                return false;
            }
            struct stat info = {};
            return stat(path, &info) == 0 && S_ISREG(info.st_mode);
        };

        int count = 0;
        struct dirent *entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (is_regular_avi(entry)) {
                ++count;
            }
        }
        if (count == 0) {
            closedir(dir);
            ESP_LOGW(ESP_UTILS_LOG_TAG, "No AVI files found in %s", dir_path);
            return ESP_ERR_NOT_FOUND;
        }

        avi_file_list = static_cast<char **>(calloc(count, sizeof(char *)));
        if (!avi_file_list) {
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }

        int loaded_count = 0;
        rewinddir(dir);
        while ((entry = readdir(dir)) != nullptr && loaded_count < count) {
            if (!is_regular_avi(entry)) {
                continue;
            }
            size_t len = strlen(dir_path) + strlen(entry->d_name) + 2;
            char *full_path = static_cast<char *>(malloc(len));
            if (!full_path) {
                for (int i = 0; i < loaded_count; ++i) {
                    free(avi_file_list[i]);
                }
                free(avi_file_list);
                avi_file_list = nullptr;
                avi_file_count = 0;
                closedir(dir);
                return ESP_ERR_NO_MEM;
            }
            snprintf(full_path, len, "%s/%s", dir_path, entry->d_name);
            avi_file_list[loaded_count++] = full_path;
        }

        closedir(dir);
        avi_file_count = loaded_count;
        ESP_LOGI(
            ESP_UTILS_LOG_TAG,
            "Loaded %d AVI file(s) from %s",
            avi_file_count,
            dir_path
        );
        return avi_file_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    /* ------------------------------------------------------------------ */
    /* 界面：视频选择网格                                                  */
    /* ------------------------------------------------------------------ */
#if VIDEO_BYTES_PER_PIXEL != 2
#error "预览网格的缩略图取样是按 RGB565 写的（硬解输出、dsc 的 cf、取样步长都要一起改）"
#endif

    bool VideoPlayer::destroySelectGrid()
    {
        if (grid_obj == nullptr) {
            return true;
        }

        /* **这里不能无限等锁** —— 本函数会在"框架要求我们停下"那条路上被
         * playTask 调到，而那时**框架正持着显示锁在等 playTask 退出**：
         *
         *   `esp_lv_adapter` 的 worker 任务就是"**持着显示锁**跑 `lv_timer_handler()`"
         *   （`esp_lv_adapter.c:1664`：`lock(-1)` → `lv_timer_handler()` → `unlock()`），
         *   而框架的手势回调正是在 `lv_timer_handler()` 里面跑的 ⇒ 它从回调里调我们的
         *   `pause()`，然后**就停在那里等我们**。
         *   ⇒ 我们再去要同一把锁 = **自锁死**，只会一直等到 `pause()` 的超时。
         *
         * 用户实测症状正好对上："下方白条上滑一次 → **卡住几秒钟不动**（= 4 s 超时）→
         * 随后因为 `pause()` 返回 false，框架 `processNavigationEvent(HOME)` 在
         * `processAppPause()` 处 `goto end`、**整个切屏被中止** → 黑屏有背光 → 要再滑一次
         * （第二次能成，是因为那时 playTask 早退出了）。"
         *
         * ⇒ 改成**带超时**拿锁；拿不到就**不删**（网格对象留在 app 的屏幕上不动），
         *   交给下一次 `showSelectGrid()`（开头会补删）或 `close()` 收掉 ——
         *   那两个上下文里 LVGL 都没有在等我们，拿锁是安全的。
         *
         * 只是"推迟"，**不是丢**：`grid_obj` 仍然指向活对象，`cells[i].img` 也仍然有效，
         *    所以下一次 showSelectGrid / close 照样能正常处理它。
         *
         * 而且**正被停下时连试都别试**：`loop_playback` 已经是 false ⇒ 框架必定
         *   正持着显示锁等我们 ⇒ 那 200 ms 是**注定失败的白等**。
         *   实测（用户 2026-10-16 报"白条上滑后有 0.5~1 秒停顿，别的 app 都很丝滑"）：
         *   日志里 `playTask stop wait = 210~270 ms`，**其中 200 ms 全花在这里**。 */
        if (!loop_playback) {
            ESP_LOGI(ESP_UTILS_LOG_TAG,
                     "Stopping: defer grid deletion to the next showSelectGrid()/close()");
            return false;
        }

        if (!bsp_extra_display_lock(200)) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "Display lock busy; defer grid deletion");
            return false;
        }

        /* 只删 LVGL 对象、**不 free 缩略图** —— 缩略图缓存留着，下次进网格秒开；
         *    它的释放统一走 freeThumbnails()（在 close() 里调）。 */
        for (int i = 0; cells && i < avi_file_count; i++) {
            cells[i].img = nullptr;
            cells[i].note = nullptr;
        }
        if (grid_obj) {
            lv_obj_del(grid_obj);
            grid_obj = nullptr;
        }
        thumb_next = 0;

        bsp_extra_display_unlock();
        return true;
    }

    void VideoPlayer::freeThumbnails()
    {
        if (!cells) {
            return;
        }
        for (int i = 0; i < avi_file_count; i++) {
            if (cells[i].px) {
                heap_caps_free(cells[i].px);
                cells[i].px = nullptr;
            }
            cells[i].fail = nullptr;
            cells[i].img = nullptr;
            cells[i].note = nullptr;
        }
        free(cells);
        cells = nullptr;
    }

    void VideoPlayer::showSelectGrid()
    {
        /* 必须在**显示旁路关着**的时候调 —— 旁路一开，屏幕上就不是 LVGL 在画了。 */
        bsp_extra_display_lock(-1);

        /* 补删上一次"被推迟"的网格（见 `destroySelectGrid()` 的说明：
         * 框架打断我们时那一次删不掉）。**不补这一步就会在旧网格上面再叠一个新网格**，
         * 旧的那些格子会一直压在底下。此刻我们不是在等谁，拿锁是安全的
         * （递归锁，`destroySelectGrid()` 里再拿一次也没问题）。 */
        if (grid_obj) {
            (void)destroySelectGrid();
        }

        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0B0E12), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

        if (!cells) {
            cells = static_cast<VideoCell *>(calloc(avi_file_count, sizeof(VideoCell)));
        }
        if (!cells) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "No memory for select grid");
            bsp_extra_display_unlock();
            return;
        }

        grid_obj = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(grid_obj);
        lv_obj_set_size(grid_obj, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_pos(grid_obj, 0, 0);
        lv_obj_set_style_bg_color(grid_obj, lv_color_hex(0x0B0E12), 0);
        lv_obj_set_style_bg_opa(grid_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(grid_obj, 16, 0);
        lv_obj_set_style_pad_row(grid_obj, 16, 0);
        lv_obj_set_style_pad_column(grid_obj, 16, 0);
        lv_obj_set_flex_flow(grid_obj, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(grid_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        /* 网格上**不做任何自定义手势** —— 退出交给框架自带的下方手势条
         * （用户 2026-10-16 拍板："用系统框架自带的，我们现在的交互方式给它让路"）。
         *
         * 为什么必须让路：框架的手势回调（`onGestureNavigationReleaseEventCallback`）
         * 和我们的 LVGL 事件**会被同一次滑动同时命中**，而框架那条路在
         * `processAppPause()` 失败时会中止**整个**"切回桌面"流程
         * ⇒ 我们这侧已经收摊、框架那侧又没切屏 ⇒ **黑屏有背光**。
         * 一条滑动只该有一个主人，所以网格上我们只管"点格子"。
         *
         * 顺带：网格上**不能**自己去读触摸（播放中那一套）——
         * `esp_lcd_touch_read_data()` 会把触摸控制器状态读走，LVGL 的 indev
         * 就再也收不到点（点击会集体失效）。所以网格上只用 LVGL 事件。 */

        lv_obj_t *title = lv_label_create(grid_obj);
        lv_label_set_text(title, "Videos - tap to play. Swipe up on the bar below to exit.");
        lv_obj_set_style_text_color(title, lv_color_hex(0xE8EAED), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_width(title, LV_PCT(100));   /* 整行占满，免得被当成一个格子排进去 */

        const int shown = avi_file_count < GRID_MAX_CELLS ? avi_file_count : GRID_MAX_CELLS;
        for (int i = 0; i < shown; i++) {
            VideoCell &c = cells[i];

            lv_obj_t *cell = lv_obj_create(grid_obj);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, GRID_CELL_W, GRID_CELL_H);
            lv_obj_set_style_radius(cell, 14, 0);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1F26), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_clip_corner(cell, true, 0);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            /* 不再要 EVENT_BUBBLE —— 那是给"从格子上起手的滑动退出"用的，
             * 而网格上的退出已经交给框架的手势条了（点格子走 CLICKED，够用）。 */

            /* 占位/失败文字（先建，图片盖在它上面；两者按需显示其中一个，照抄相册） */
            c.note = lv_label_create(cell);
            lv_obj_set_width(c.note, GRID_THUMB_MAX_W - 8);
            lv_label_set_long_mode(c.note, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(c.note, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(c.note, lv_color_hex(0xFF8A80), 0);
            lv_obj_set_style_text_font(c.note, &lv_font_montserrat_16, 0);
            lv_obj_align(c.note, LV_ALIGN_TOP_MID, 0, GRID_THUMB_MAX_H / 2 - 20);

            c.img = lv_image_create(cell);
            lv_obj_set_size(c.img, GRID_THUMB_MAX_W, GRID_THUMB_MAX_H);
            lv_obj_align(c.img, LV_ALIGN_TOP_MID, 0, 6);
            lv_image_set_inner_align(c.img, LV_IMAGE_ALIGN_CONTAIN);
            /* 还没小图、又没写原因 ⇒ "等着补"，先藏起来，别显示成一张空图 */
            lv_obj_add_flag(c.img, LV_OBJ_FLAG_HIDDEN);

            /* 文件名只取 basename，且**必须全 ASCII** ——
             * montserrat 一个中文字形都没有，画出来是静默乱码。 */
            const char *base = strrchr(avi_file_list[i], '/');
            base = base ? base + 1 : avi_file_list[i];
            lv_obj_t *cap = lv_label_create(cell);
            lv_label_set_text(cap, base);
            lv_obj_set_style_text_color(cap, lv_color_hex(0xC7CBD1), 0);
            lv_obj_set_style_text_font(cap, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(cap, GRID_CELL_W - 12);
            lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
            lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -8);

            lv_obj_add_event_cb(cell, onCellClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        }

        /* 底部一行：文件数 + PSRAM 余量。截断要**明说**。 */
        lv_obj_t *foot = lv_label_create(grid_obj);
        lv_obj_set_width(foot, LV_PCT(100));
        if (shown < avi_file_count) {
            lv_label_set_text_fmt(
                foot, "SHOWING FIRST %d OF %d (over grid limit)   PSRAM free %.1f MB",
                shown, avi_file_count,
                static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / (1024.0 * 1024.0));
            lv_obj_set_style_text_color(foot, lv_color_hex(0xFF6B6B), 0);
        } else {
            lv_label_set_text_fmt(
                foot, "%d video(s)   PSRAM free %.1f MB", avi_file_count,
                static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / (1024.0 * 1024.0));
            lv_obj_set_style_text_color(foot, lv_color_hex(0x6B7280), 0);
        }
        lv_obj_set_style_text_font(foot, &lv_font_montserrat_16, 0);

        /* 把**已经缓存好**的缩略图挂上去（回网格时秒开，不用重算）。
         * 用 applyThumbToCell()：它自己会加锁，而这里本来就持着锁 —— 递归锁嵌套安全。 */
        for (int i = 0; i < shown; i++) {
            applyThumbToCell(i);
        }
        thumb_next = 0;
        grid_built_us = esp_timer_get_time();

        bsp_extra_display_unlock();
    }

    void VideoPlayer::fillNextThumb()
    {
        if (!cells || !avi_file_list) {
            return;
        }
        if (thumb_next >= avi_file_count || thumb_next >= GRID_MAX_CELLS) {
            return;
        }

        const int i = thumb_next;
        VideoCell &c = cells[i];
        if (c.img == nullptr || c.px != nullptr || c.fail != nullptr) {
            thumb_next = i + 1;
            applyThumbToCell(i);
            return;
        }

        /* 1) 读该文件的第一个视频块 */
        uint8_t *jpeg = nullptr;
        size_t jlen = 0;
        if (!_firstVideoFrame(avi_file_list[i], &jpeg, &jlen)) {
            c.fail = "no video frame";
            thumb_next = i + 1;
            applyThumbToCell(i);
            return;
        }

        /* 2) 硬件解码（P4 的硬解**不支持缩放** ⇒ 只能整帧解出来再自己抽样，
         *    见板子的格式约束）。解码输出由驱动自己做 cache 失效
         *    （esp_driver_jpeg/jpeg_decode.c:313 `esp_cache_msync(..., DIR_M2C)`）
         *    ⇒ CPU 直接读它是安全的，不用我们再同步一次。 */
        jpeg_decode_picture_info_t info;
        if (jpeg_decoder_get_info(jpeg, jlen, &info) != ESP_OK ||
                info.width == 0 || info.height == 0 ||
                initJpegDecoder() != ESP_OK) {
            free(jpeg);
            c.fail = "bad jpeg";
            thumb_next = i + 1;
            applyThumbToCell(i);
            return;
        }

        uint32_t dec_w = 0;
        uint32_t dec_h = 0;
        mcuAlignedSize(info, &dec_w, &dec_h);

        if (ensureJpegOutputBuffer(static_cast<size_t>(dec_w) * dec_h * VIDEO_BYTES_PER_PIXEL)
                != ESP_OK) {
            free(jpeg);
            c.fail = "out of memory";
            thumb_next = i + 1;
            applyThumbToCell(i);
            return;
        }

        jpeg_decode_cfg_t dec_cfg = {};
        dec_cfg.output_format = VIDEO_JPEG_OUT_FORMAT;
        dec_cfg.rgb_order = VIDEO_JPEG_RGB_ORDER;
        dec_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

        uint32_t out_size = 0;
        const esp_err_t dret = jpeg_decoder_process(
                                   jpeg_handle, &dec_cfg, jpeg, static_cast<uint32_t>(jlen),
                                   jpeg_output_buffer,
                                   static_cast<uint32_t>(jpeg_output_buffer_size), &out_size);
        free(jpeg);
        if (dret != ESP_OK) {
            c.fail = "decode failed";
            thumb_next = i + 1;
            applyThumbToCell(i);
            return;
        }

        /* 3) 按源画面比例算缩略图尺寸（塞进 224×126 的框内），保证 LVGL 不用缩放 */
        int tw = GRID_THUMB_MAX_W;
        int th = GRID_THUMB_MAX_H;
        if (static_cast<int64_t>(info.width) * GRID_THUMB_MAX_H >
                static_cast<int64_t>(info.height) * GRID_THUMB_MAX_W) {
            th = static_cast<int>(static_cast<int64_t>(info.height) * GRID_THUMB_MAX_W / info.width);
        } else {
            tw = static_cast<int>(static_cast<int64_t>(info.width) * GRID_THUMB_MAX_H / info.height);
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;

        uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(
                static_cast<size_t>(GRID_THUMB_MAX_W) * GRID_THUMB_MAX_H * VIDEO_BYTES_PER_PIXEL,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf == nullptr) {
            c.fail = "out of memory";
            thumb_next = i + 1;
            applyThumbToCell(i);
            return;
        }

        /* 4) 最近邻抽样。源行的跨距是 **MCU 对齐后的 dec_w**，不是 info.width。 */
        const uint16_t *src = reinterpret_cast<const uint16_t *>(jpeg_output_buffer);
        uint16_t *dst = reinterpret_cast<uint16_t *>(buf);
        for (int y = 0; y < th; y++) {
            const uint32_t sy = static_cast<uint32_t>(
                                    static_cast<int64_t>(y) * info.height / th);
            const uint16_t *srow = src + static_cast<size_t>(sy) * dec_w;
            uint16_t *drow = dst + static_cast<size_t>(y) * tw;
            for (int x = 0; x < tw; x++) {
                const uint32_t sx = static_cast<uint32_t>(
                                        static_cast<int64_t>(x) * info.width / tw);
                drow[x] = srow[sx];
            }
        }

        c.px = buf;
        memset(&c.dsc, 0, sizeof(c.dsc));
        c.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        c.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        c.dsc.header.w = static_cast<uint32_t>(tw);
        c.dsc.header.h = static_cast<uint32_t>(th);
        c.dsc.header.stride = static_cast<uint32_t>(tw * VIDEO_BYTES_PER_PIXEL);
        c.dsc.data_size = static_cast<uint32_t>(tw * th * VIDEO_BYTES_PER_PIXEL);
        c.dsc.data = buf;

        thumb_next = i + 1;
        applyThumbToCell(i);
    }

    /* 把一个格子的结果挂到 LVGL 上。
     *
     * 为什么**没有`lv_timer`**：第一版照抄相册用了 `lv_timer_create(onThumbTick, ...)`，
     * 结果每次关 app 必崩（Load access fault）。根因是框架的记账机制 ——
     * `App::endRecordResource()` 会把 **`run()` 期间**建的屏幕/定时器记下来，
     * 关 app 时由 `cleanRecordResource()` 再删一遍；而 `playTask` 恰恰是 `run()` 的
     * 最后一句启动的，我们那个定时器**正好落在记账窗口内**
     * ⇒ 我们删一次、框架再删一次 ⇒ 定时器链损坏 ⇒ 之后在 LVGL 内部随机崩
     * （实测 7 次崩溃 MEPC 全同：`lv_event_mark_deleted` ← `lv_obj_destructor`）。
     * ⇒ 缩略图改由 `playTask` 的网格等待循环一拍一张地补，**一个 LVGL 定时器都不建**。 */
    void VideoPlayer::applyThumbToCell(int i)
    {
        if (!cells || !grid_obj || i < 0 || i >= avi_file_count) {
            return;     /* 兜底：网格已经拆了（切去播放 / 退出 app），别碰任何 lv 对象 */
        }
        VideoCell &c = cells[i];
        if (c.img == nullptr) {
            return;
        }

        /* 同上：**不能无限等锁**（理由见 `destroySelectGrid()` 的注释 ——
         * 框架打断我们时，显示锁正被它攥着等我们退出）。
         * 这里是"补缩略图"，纯装饰 ⇒ 拿不到就这一拍跳过。
         * 但**必须把 `thumb_next` 退回去**（调用点都是"先 +1 再调本函数"），
         *    否则这一格的缩略图就永远补不上了。 */
        if (!bsp_extra_display_lock(200)) {
            thumb_next = i;
            return;
        }
        if (c.px) {
            lv_image_set_src(c.img, &c.dsc);
            lv_obj_clear_flag(c.img, LV_OBJ_FLAG_HIDDEN);
        } else if (c.fail && c.note) {
            lv_label_set_text(c.note, c.fail);
        }
        bsp_extra_display_unlock();
    }

    void VideoPlayer::onCellClicked(lv_event_t *e)
    {
        if (_instance == nullptr) {
            return;
        }
        /* 刚建好网格的头 500 ms 里，忽略点击 ——
         * 用户"点 app 图标"那一下的触摸有可能还压在屏上，网格一建出来就落在某个格子上，
         * 于是**一进 app 就直接开播第一个**（看起来像"网格没生效"）。
         * 这是防呆，不是防业务：真点格子是在网格显示之后才发生的。 */
        if (esp_timer_get_time() - _instance->grid_built_us < 500000) {
            return;
        }
        /* 这里**只写一个变量** —— 不删对象、不建任务、不切屏幕。
         * 事件回调是在 LVGL 遍历对象树的过程中跑的，"在自己的回调里把自己所在的那棵树
         * 删掉"是经典雷（用后释放）。
         * 真正的切换交给 playTask 的轮询循环，它每 30 ms 看一次这个变量。 */
        _instance->grid_pick =
            static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    }

    esp_err_t VideoPlayer::initDisplayBypass()
    {
#ifdef CONFIG_CACHE_L2_CACHE_LINE_SIZE
        data_cache_line_size = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
#else
        data_cache_line_size = 64;
#endif

        display = lv_display_get_default();
        ESP_RETURN_ON_FALSE(display != nullptr, ESP_ERR_INVALID_STATE, ESP_UTILS_LOG_TAG, "LVGL display is not ready");

        display_panel = bsp_display_get_panel_handle();
        ESP_RETURN_ON_FALSE(display_panel != nullptr, ESP_ERR_INVALID_STATE, ESP_UTILS_LOG_TAG, "LCD panel is not ready");

        esp_err_t ret = ESP_OK;
#if CONFIG_BSP_LCD_DPI_BUFFER_NUMS >= 3
        ret = esp_lcd_dpi_panel_get_frame_buffer(
                  display_panel,
                  CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
                  &lcd_buffers[0],
                  &lcd_buffers[1],
                  &lcd_buffers[2]
              );
#elif CONFIG_BSP_LCD_DPI_BUFFER_NUMS == 2
        ret = esp_lcd_dpi_panel_get_frame_buffer(
                  display_panel,
                  CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
                  &lcd_buffers[0],
                  &lcd_buffers[1]
              );
#else
#error "VideoPlayer needs at least two LCD frame buffers"
#endif
        ESP_RETURN_ON_ERROR(ret, ESP_UTILS_LOG_TAG, "Get LCD frame buffers failed");
        current_buf_idx = 0;

        if (ppa_srm_handle == nullptr) {
            ppa_client_config_t ppa_srm_config = {
                .oper_type = PPA_OPERATION_SRM,
            };
            ESP_RETURN_ON_ERROR(
                ppa_register_client(&ppa_srm_config, &ppa_srm_handle),
                ESP_UTILS_LOG_TAG,
                "Register PPA SRM failed"
            );
        }

        /* 临时验证：这是**播放期间用的第二个触摸实例**，它也必须跟着转，
         * 否则"网格上触摸对、一进播放就错位"。
         * 上面那个 `.rotation` 是**死字段** —— `bsp_touch_new()` 只读 `cfg->touch_flags.*`，
         *    压根不看 `cfg->rotation`（`esp32_p4_wifi6_touch_lcd_x.c:1271-1285`）。
         *    旋转这件事对这个实例只由下面三个 flag 决定，所以这里**故意保持 ROTATE_0**、
         *    只改 flag（改那个字段不会有任何效果，只会误导读者）。
         * 取值与 main.cpp 里那份**逐位相同**，推导见 main.cpp 的注释块。
         * main.cpp 的横屏从 ROTATE_90 翻成 ROTATE_270 之后，
         *    这三个 flag 必须**跟着一起翻转**，否则就是"网格上触摸对、一进播放就错位"
         *    —— 这里是硬编码的副本，不是从 main.cpp 读的（两处改动要同步）。 */
        bsp_display_cfg_t touch_cfg = {
            .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
            .rotation = ESP_LV_ADAPTER_ROTATE_0,        /* 死字段，别改（见上） */
            .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
            .touch_flags = {                            /* 第二个触摸实例 */
                .swap_xy = 1,
                .mirror_x = 0,
                .mirror_y = 1,
            },
        };
        ret = bsp_touch_new(&touch_cfg, &touch_handle);
        if (ret != ESP_OK) {
            deinitDisplayBypass();
            return ret;
        }

        ret = esp_lv_adapter_set_dummy_draw(display, true);
        if (ret != ESP_OK) {
            deinitDisplayBypass();
            return ret;
        }
        dummy_enabled = true;

        ret = esp_lv_adapter_pause(-1);
        if (ret != ESP_OK) {
            deinitDisplayBypass();
            return ret;
        }
        lvgl_paused = true;

        /* 先等一下，**再**清缓冲。
         * `esp_lv_adapter_pause()` 返回 ≠ LVGL 那次刷新已经落地：可能在途的 flush 会在它
         * 返回之后才写进某个帧缓冲 —— 那样我们清完黑，它又把网格写回去，**那一个缓冲此后
         * 一直是旧的网格**，面板每轮到它就闪一下（实测症状："视频在正常播，上方预览界面的
         * 残影一直在闪"）。快速连续切换时更容易撞上在途刷新，所以概率不高但会出现。 */
        vTaskDelay(pdMS_TO_TICKS(60));

        /* LVGL 停了，但它**刚把预览网格画在这三个 DPI 帧缓冲里**。这里立刻全部清黑。
         * 为什么不能只靠 videoCallback 里那次清黑：那次只在"构图变了"时做，
         * 而且它在**第一帧**才跑 —— 首帧要等 avi_player 解完文件头（几百毫秒），
         * 这段时间面板会一直扫到**残留的网格**。快速连续切换时撞上的概率更大
         * （实测症状：视频在正常播，**上方一直有预览界面的残影在闪**）。
         * 清在这里 ⇒ 一进播放就是干净的黑，"扫到还没写过的缓冲"最多黑一下。 */
        for (int i = 0; i < CONFIG_BSP_LCD_DPI_BUFFER_NUMS; i++) {
            if (lcd_buffers[i]) {
                const size_t bytes =
                    static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT * VIDEO_BYTES_PER_PIXEL;
                memset(lcd_buffers[i], 0, bytes);
                /* 同样要立刻写回 PSRAM —— 道理见 videoCallback 里构图切换那段。
                 * 这里因为离第一帧 PPA 还有几百毫秒，通常靠自然换出也能落到 PSRAM；
                 * 但"通常"不是保证，显式刷一次才与构图切换那条路语义一致。 */
                esp_cache_msync(lcd_buffers[i], bytes,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            }
            /* 全部缓冲都是全黑 ⇒ 每个缓冲的"当前画面块"记为空（没有残留要清） */
            buf_last_blk[i] = BlkRect {};
        }
        /* 与上面配套：让 videoCallback 的头一帧按新尺寸重算一次构图 */
        last_video_width = 0;
        last_video_height = 0;
        last_video_rot = -1;            /* -1 ⇒ 和任何一档都不相等，头一帧必重算 */
        last_video_crop_x = 0;
        last_video_crop_w = 0;
        /* 进播放时重新检测补边（每部片子会再清一次，见 playTask 的换片处） */
        pb_left = 0;
        pb_width = 0;
        pb_cand_left = 0;
        pb_cand_w = 0;
        pb_agree = 0;
        pb_attempts = 0;
        pb_done = false;
        rot_requests = 0;
        last_tap_us = 0;
        /* `video_rot_idx` **故意不重置** —— 它是用户选的档位，跨次播放应该记住。 */

        ESP_LOGI(
            ESP_UTILS_LOG_TAG,
            "Video display bypass: %dx%d, %d frame buffers",
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            CONFIG_BSP_LCD_DPI_BUFFER_NUMS
        );
        return ESP_OK;
    }

    void VideoPlayer::deinitDisplayBypass()
    {
        /* **第一件事就是把"我还在"的两张凭据一起撤掉**（`display` 和 `dummy_enabled`）。
         * 帧任务（avi_player 的 30 Hz 定时器）**不受我们控制**，随时可能有一个迟到的
         * `videoCallback` 正在跑；它开头那两判就是查这两个字段。
         *
         * 实测崩溃链（"视频播放时滑动 ⇒ 复位重启"）：原来 `display = nullptr` 排在
         * **整个函数的最后**，于是存在一个"`lcd_buffers[]` 已被清零、而 `display` 还非空"的窗口
         * ⇒ 迟到的回调穿过判据一路走到清缓冲那句 ⇒ `memset(NULL, …)` ⇒ `Store access fault`。
         * 栈证据：`RA → VideoPlayer.cpp:1610`（memset 的返回点）、`A0 = 0`、
         * `A3 = 0x1f4000`（= 800*1280*2，正好一个帧缓冲）。
         * ⇒ 这两行必须**先执行、且先于任何拆解动作**。 */
        lv_display_t *const disp = display;
        const bool had_dummy = dummy_enabled;
        display = nullptr;
        dummy_enabled = false;

        if (had_dummy && disp) {
            esp_lv_adapter_set_dummy_draw(disp, false);
        }

        if (lvgl_paused) {
            esp_lv_adapter_resume();
            lvgl_paused = false;
        }

        if (touch_handle) {
            /* 必须用 `bsp_touch_del()`：`esp_lcd_touch_del()` **不释放**
             * `bsp_touch_new()` 内部建的那个 I2C panel-io ⇒ 每播一次漏一个，
             * 而它会把 PSRAM 的大块永久钉碎（会连累相册大图 / 相机预览）。
             * 详见 bsp/touch.h 的说明。 */
            bsp_touch_del(touch_handle);   /* 泄漏修复 */
            touch_handle = nullptr;
        }

        memset(lcd_buffers, 0, sizeof(lcd_buffers));
        display_panel = nullptr;
        display = nullptr;
        current_buf_idx = 0;
        touch_active = false;
        last_video_width = 0;
        last_video_height = 0;
        last_video_rot = -1;
        last_video_crop_x = 0;
        last_video_crop_w = 0;
        pb_left = 0;
        pb_width = 0;
        pb_cand_left = 0;
        pb_cand_w = 0;
        pb_agree = 0;
        pb_attempts = 0;
        pb_done = false;
        rot_requests = 0;
        last_tap_us = 0;
    }

    esp_err_t VideoPlayer::initJpegDecoder()
    {
        if (jpeg_handle != nullptr)
            return ESP_OK;

        /* 硬件解码引擎。timeout_ms 按驱动头文件的说法要"大于单帧合法解码时间"，
         * 它给的例子是 30fps ⇒ 必须 > 34 ms。给 1000 ms 留足余量 —— 这个值同时是
         * **故障时的兜底**：真卡住也会在 1 秒后返回 ESP_ERR_TIMEOUT，而不是永久挂起。 */
        jpeg_decode_engine_cfg_t eng_cfg = {
            .intr_priority = 0,      /* 0 = 让驱动自选 */
            .timeout_ms = 1000,
        };
        return jpeg_new_decoder_engine(&eng_cfg, &jpeg_handle);
    }

    void VideoPlayer::deinitJpegDecoder()
    {
        if (jpeg_handle)
        {
            jpeg_del_decoder_engine(jpeg_handle);
            jpeg_handle = nullptr;
        }
    }

    esp_err_t VideoPlayer::ensureJpegOutputBuffer(size_t out_len)
    {
        if (out_len == 0) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (jpeg_output_buffer && jpeg_output_buffer_size >= out_len) {
            return ESP_OK;
        }

        releaseJpegOutputBuffer();
        /* **必须**用 jpeg_alloc_decoder_mem，不能自己 heap_caps_aligned_calloc。
         * 驱动在 jpeg_decoder_process 入口会校验输出缓冲（jpeg_decode.c:265-269），
         * 而且真正的原因是缓存失效的粒度：输出缓冲由 2D-DMA 直接写入 PSRAM，
         * 驱动前后要对它做 esp_cache_msync 的 M2C 失效（jpeg_decode.c:313/333），
         * 那要求**地址和长度都按 cache line 对齐** —— 本板 L2 cache line = 128 B
         * （sdkconfig: CONFIG_CACHE_L2_CACHE_LINE_128B=y），原来那套 16 字节对齐会被拒。
         * jpeg_alloc_decoder_mem 就是替我们把这层对齐做好的（jpeg_decode.c:387-404：
         * 输出方向 = aligned_calloc(cache_align, ...) 且 size 也向上取整到 cache_align）。 */
        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t alloc_size = 0;
        jpeg_output_buffer = static_cast<uint8_t *>(
                                jpeg_alloc_decoder_mem(out_len, &mem_cfg, &alloc_size));
        if (!jpeg_output_buffer) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "JPEG output buffer allocation failed: %u bytes",
                     static_cast<unsigned>(out_len));
            return ESP_ERR_NO_MEM;
        }

        jpeg_output_buffer_size = alloc_size;
        return ESP_OK;
    }

    void VideoPlayer::releaseJpegOutputBuffer()
    {
        if (jpeg_output_buffer) {
            heap_caps_free(jpeg_output_buffer);
            jpeg_output_buffer = nullptr;
            jpeg_output_buffer_size = 0;
        }
    }

    /* ------------------------------------------------------------------
     * PPA SRM 的定点缩放：**小数位只有 4 bit（1/16 精度）**。
     * 依据（都在磁盘上核过，不是推断）：
     *   · `hal/esp32p4/include/hal/ppa_ll.h:29`
     *     `#define PPA_LL_SRM_SCALING_FRAG_MAX (PPA_SR_SCAL_X_FRAG_V + 1)`
     *   · `soc/esp32p4/register/hw_ver3/soc/ppa_reg.h:779` `PPA_SR_SCAL_X_FRAG_V = 0x0F`
     *     ⇒ FRAG_MAX = 16 ⇒ 小数步进就是 1/16
     *   · `esp_driver_ppa/src/ppa_srm.c:230`
     *     `frag = (uint32_t)(scale * 16) & 15`
     *   · `ppa_srm.c:236-240` 又用 int/frag **重算**输出块尺寸（整数除法）：
     *     `new_block = int*in + (frag*in)/16`
     * ⇒ 请求 1.6 实际只会得到 1 + 9/16 = **1.5625**。
     * 不注意这一条，"把黑边居中"就会按错的块宽去算 —— 而错的那几像素
     * 会**全部堆到右边和下边**（改前实测：左 40 / 右 57、上 0 / 下 30）。
     * ------------------------------------------------------------------ */
    static inline uint32_t ppaScaleIntPart(float s)
    {
        return static_cast<uint32_t>(s);
    }
    static inline uint32_t ppaScaleFragPart(float s)
    {
        return static_cast<uint32_t>(s * 16.0f) & 15u;
    }
    static inline uint32_t ppaScaledLen(uint32_t len, uint32_t ip, uint32_t fp)
    {
        return ip * len + (fp * len) / 16u;   /* 整数除法，与驱动一致 */
    }

    /* 曾经的 `clearLetterboxBorders()`（只清四条黑边）已删除 —— 实测横屏下黑边会持续
     * 频闪竖屏残留（详见 hpp 里"构图变化时清缓冲"的说明）。别再写回去。 */

    void VideoPlayer::videoCallback(frame_data_t *data, void *arg)
    {
        if (!data || !data->data || data->data_bytes == 0)
            return;


        VideoPlayer *self = static_cast<VideoPlayer *>(arg);
        if (data->data_bytes > self->max_chunk_seen) {
            self->max_chunk_seen = data->data_bytes;   /* 盯"最大 chunk vs 读缓冲"的余量 */
        }
        const int next_buf_idx =
            (self->current_buf_idx + 1) % CONFIG_BSP_LCD_DPI_BUFFER_NUMS;

        /* `!self->display` 必须一起判：`avi_player_play_stop()` 是**异步**的
         * （只置事件位），我们可能已经开始拆显示了，它还会再送 1~2 帧过来。
         * 缺这一判的后果实测就是这个：`esp_lv_adapter_dummy_draw_blit: Invalid display handle`
         * + `ppa_do_scale_rotate_mirror: invalid out.buffer addr`（都是迟到的帧在踩已拆掉的东西）。 */
        if (self->initJpegDecoder() != ESP_OK ||
                !self->display ||
                !self->lcd_buffers[next_buf_idx] ||
                !self->dummy_enabled || !self->ppa_srm_handle) {
            return;
        }

        // Decode each MJPEG frame into a PSRAM buffer, then use PPA SRM to scale it
        // into the next LCD frame buffer without involving LVGL widgets.
        jpeg_decode_picture_info_t info;
        if (jpeg_decoder_get_info(data->data, data->data_bytes, &info) != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "JPEG header parse failed");
            return;
        }

        if (info.width == 0 || info.height == 0) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Invalid JPEG frame size: %ux%u",
                     (unsigned)info.width, (unsigned)info.height);
            return;
        }

        /* 解码缓冲的行宽**不是** info.width，而是 MCU 对齐后的宽度（见 mcuAlignedSize 的说明）。
         * 填错的后果不是报错、而是**画面斜着错行**，所以这两个值一定要用对齐后的。 */
        uint32_t dec_w = 0;
        uint32_t dec_h = 0;
        mcuAlignedSize(info, &dec_w, &dec_h);

        if (self->ensureJpegOutputBuffer(static_cast<size_t>(dec_w) * dec_h * VIDEO_BYTES_PER_PIXEL)
                != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Prepare JPEG output buffer failed");
            return;
        }

        jpeg_decode_cfg_t dec_cfg = {};
        dec_cfg.output_format = VIDEO_JPEG_OUT_FORMAT;
        dec_cfg.rgb_order = VIDEO_JPEG_RGB_ORDER;
        dec_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

        uint32_t out_size = 0;
        if (jpeg_decoder_process(self->jpeg_handle, &dec_cfg,
                                 data->data, data->data_bytes,
                                 self->jpeg_output_buffer,
                                 static_cast<uint32_t>(self->jpeg_output_buffer_size),
                                 &out_size) != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "JPEG decode process failed");
            return;
        }

        /* ---- 画面构图：**保比例 + 居中 + 黑边**（letterbox / "contain"）----------------
         * 原实现是"铺满 + 裁切"（cover）：1280x720 的横屏视频要塞进 800x1280 的竖屏，
         * 就把中间 450x720 那一条裁出来再放大 2.8 倍 —— **源画面 65% 的像素直接丢掉**，
         * 用户实测的评价就是"视频画质确实一般"。
         * 改成 letterbox 后**一像素不丢**，代价是上下（横屏源）或左右（竖屏源）留黑边。
         *
         * 黑边区域 PPA **永远不会去写**，所以只需在"构图变了"的时候把所有 LCD 帧缓冲
         *    清一次黑（见下面那段），之后就一直是黑的 —— **每帧零成本**，不用每帧 memset。 */
        const uint32_t src_w = info.width ? info.width : 1;
        const uint32_t src_h = info.height ? info.height : 1;

        /* 画面旋转的**唯一写者**就在这里（播放任务那条路只往 `rot_requests` 里加数，
         * 不碰 `video_rot_idx`）⇒ 不需要加锁。双击一次 = 换下一档。 */
        if (self->rot_requests > 0) {
            self->rot_requests = 0;
            self->video_rot_idx = (self->video_rot_idx + 1) % 3;
            ESP_LOGW(ESP_UTILS_LOG_TAG, "video rotate -> %d deg",
                     kVideoRotDeg[self->video_rot_idx]);
        }
        const int vid_rot = kVideoRotDeg[self->video_rot_idx];

        /* 「左右补黑边的竖屏片」自动去边（**只在 0° 档生效**）。
         * 检测在每部片子的前几帧里做，连续若干帧结果一致才采纳；没检出就保持原样。 */
        uint32_t crop_x = 0;
        uint32_t crop_y = 0;
        uint32_t crop_w = src_w;
        uint32_t crop_h = src_h;
        if (vid_rot == 0 && !self->pb_done && self->pb_attempts < PB_MAX_ATTEMPTS) {
            const PillarboxMeasure m =
                measurePillarbox(self->jpeg_output_buffer, dec_w, src_w, src_h);
            self->pb_attempts++;
            if (m.cls == PillarboxClass::Unknown) {
                /* 淡入/黑场：既不算"有"也不算"没有"，下一帧再试（不计入一致帧数） */
            } else if (m.cls == PillarboxClass::Found) {
                if (self->pb_cand_w > 0 && self->pb_agree > 0 &&
                        intDiff(static_cast<int>(m.left), self->pb_cand_left) <= 2 &&
                        intDiff(static_cast<int>(m.width), self->pb_cand_w) <= 2) {
                    self->pb_agree++;
                } else {
                    self->pb_agree = 1;
                }
                self->pb_cand_left = static_cast<int>(m.left);
                self->pb_cand_w = static_cast<int>(m.width);
            } else {   /* PillarboxClass::None */
                if (self->pb_cand_w == 0 && self->pb_agree > 0) {
                    self->pb_agree++;
                } else {
                    self->pb_agree = 1;
                }
                self->pb_cand_left = 0;
                self->pb_cand_w = 0;
            }

            const int need = (self->pb_cand_w > 0) ? PB_FOUND_AGREE : PB_NONE_AGREE;
            if (self->pb_agree >= need) {
                self->pb_done = true;
                if (self->pb_cand_w > 0) {
                    self->pb_left = self->pb_cand_left;
                    self->pb_width = self->pb_cand_w;
                    ESP_LOGI(ESP_UTILS_LOG_TAG,
                             "Pillarbox detected: src %ux%u -> content %ux%u at x=%d "
                             "(bars %d/%d), fill portrait screen",
                             (unsigned)src_w, (unsigned)src_h,
                             (unsigned)self->pb_width, (unsigned)src_h,
                             self->pb_left, self->pb_left,
                             (int)src_w - self->pb_left - self->pb_width);
                } else {
                    ESP_LOGI(ESP_UTILS_LOG_TAG,
                             "No pillarbox in src %ux%u, keep original fit",
                             (unsigned)src_w, (unsigned)src_h);
                }
            }
        }
        if (vid_rot == 0 && self->pb_width > 0) {
            crop_x = static_cast<uint32_t>(self->pb_left);
            crop_w = static_cast<uint32_t>(self->pb_width);
        }

        /* 旋转之后"观众看到的那一版源"的尺寸：90°/270° ⇒ 横竖互换。
         * 之后仍走原来那套 letterbox 规则 —— 这样**两种情形共用一份构图代码**。
         * 0° 档用**裁掉补边后的内容区**参与计算；旋转档仍用整幅源。 */
        const uint32_t vw = (vid_rot == 0) ? crop_w : src_h;
        const uint32_t vh = (vid_rot == 0) ? crop_h : src_w;

        uint32_t dst_w = 0;
        uint32_t dst_h = 0;
        if (static_cast<uint64_t>(vw) * DISPLAY_HEIGHT >=
                static_cast<uint64_t>(vh) * DISPLAY_WIDTH) {
            /* 源比屏幕更"宽"（横屏源进竖屏）⇒ 宽度顶满，上下留黑边 */
            dst_w = DISPLAY_WIDTH;
            dst_h = static_cast<uint32_t>(
                        static_cast<uint64_t>(vh) * DISPLAY_WIDTH / vw);
        } else {
            /* 源比屏幕更"瘦"（竖屏源）⇒ 高度顶满，左右留黑边 */
            dst_h = DISPLAY_HEIGHT;
            dst_w = static_cast<uint32_t>(
                        static_cast<uint64_t>(vw) * DISPLAY_HEIGHT / vh);
        }
        dst_w = dst_w ? dst_w : 1;
        dst_h = dst_h ? dst_h : 1;

        /* 缩放比的算法**随旋转而不同** —— 不是笔误。
         * `ppa_srm.c:233-244` 明确按旋转分支算输出块：
         *   angle 0/180 ⇒ new_block_w = scale_x * in.block_w，new_block_h = scale_y * in.block_h
         *   angle 90/270 ⇒ new_block_w = scale_y * in.block_h，new_block_h = scale_x * in.block_w
         * 也就是说 **转 90° 时 PPA 把输出块转置了**：scale_x 作用在"输入宽"上、
         * 最终变成输出的**高**。所以要得到 new_block = (dst_w, dst_h)，就必须
         *   scale_x = dst_h / src_w、scale_y = dst_w / src_h。
         * 把这一条写反的后果不是报错，而是**画面被拉伸**（但 PPA 的越界检查可能先拦下来）。 */
        const float scale_x = (vid_rot == 0)
                              ? static_cast<float>(dst_w) / static_cast<float>(crop_w)
                              : static_cast<float>(dst_h) / static_cast<float>(src_w);
        const float scale_y = (vid_rot == 0)
                              ? static_cast<float>(dst_h) / static_cast<float>(crop_h)
                              : static_cast<float>(dst_w) / static_cast<float>(src_h);

        /* 上面那个 `dst_w/dst_h` 只是**理想**块（连续缩放）。PPA 实际只会得到
         * **量化到 1/16** 的块（见 ppaScaleIntPart 那一大段说明）⇒ 真正决定黑边位置的
         * 是下面这对 `blk_w/blk_h`，不是 dst_w/dst_h。 */
        const uint32_t sx_i = ppaScaleIntPart(scale_x), sx_f = ppaScaleFragPart(scale_x);
        const uint32_t sy_i = ppaScaleIntPart(scale_y), sy_f = ppaScaleFragPart(scale_y);
        const uint32_t blk_w = (vid_rot == 0) ? ppaScaledLen(crop_w, sx_i, sx_f)
                                              : ppaScaledLen(src_h, sy_i, sy_f);
        const uint32_t blk_h = (vid_rot == 0) ? ppaScaledLen(crop_h, sy_i, sy_f)
                                              : ppaScaledLen(src_w, sx_i, sx_f);

        /* 按**实际**块居中 ⇒ 左右/上下黑边相等（差最多 1 px 的取整） */
        const uint32_t dst_x = (DISPLAY_WIDTH - blk_w) / 2;
        const uint32_t dst_y = (DISPLAY_HEIGHT - blk_h) / 2;

        if (self->last_video_width != info.width || self->last_video_height != info.height ||
                self->last_video_rot != vid_rot ||
                self->last_video_crop_x != crop_x || self->last_video_crop_w != crop_w) {
            /* 构图变了 ⇒ 黑边的位置/宽度也变了（日志用；真正的清理由下面
             * 按缓冲逐个算"旧块 − 新块"的残留区域完成，见 clearLeftoverRect()）。
             * `crop_*` 也参与判据：检测采纳时画面构图会变，同样要清。 */
            ESP_LOGI(ESP_UTILS_LOG_TAG,
                     "Fit video frame (letterbox): src %ux%u rot %d crop %ux%u@%u -> "
                     "blk %ux%u at (%u,%u) | 理想块 %ux%u scale %.4fx%.4f → PPA 定点 %.4fx%.4f",
                     (unsigned)src_w, (unsigned)src_h, vid_rot,
                     (unsigned)crop_w, (unsigned)crop_h, (unsigned)crop_x,
                     (unsigned)blk_w, (unsigned)blk_h,
                     (unsigned)dst_x, (unsigned)dst_y,
                     (unsigned)dst_w, (unsigned)dst_h, scale_x, scale_y,
                     static_cast<float>(sx_i) + static_cast<float>(sx_f) / 16.0f,
                     static_cast<float>(sy_i) + static_cast<float>(sy_f) / 16.0f);
            self->last_video_width = info.width;
            self->last_video_height = info.height;
            self->last_video_rot = vid_rot;
            self->last_video_crop_x = static_cast<uint16_t>(crop_x);
            self->last_video_crop_w = static_cast<uint16_t>(crop_w);
        }

        void *lcd_buffer = self->lcd_buffers[next_buf_idx];

        /* 画之前先把这个缓冲里「旧块 − 新块」的残留清掉。
         * 每个缓冲各管各的（`buf_last_blk[]`），所以"构图连续变化"也天然正确：
         * 没轮到的缓冲还记着它自己的旧块，轮到它时按最新的新块算差集。 */
        {
            const BlkRect cur_blk = {
                static_cast<uint16_t>(dst_x), static_cast<uint16_t>(dst_y),
                static_cast<uint16_t>(blk_w), static_cast<uint16_t>(blk_h)
            };
            const BlkRect &prev_blk = self->buf_last_blk[next_buf_idx];
            clearLeftoverRect(lcd_buffer, prev_blk, cur_blk);
            self->buf_last_blk[next_buf_idx] = cur_blk;
        }

        ppa_srm_oper_config_t srm_config = {};
        srm_config.in.buffer = self->jpeg_output_buffer;
        /* pic_w/pic_h 填的是**解码缓冲的跨距**，不是画面的逻辑尺寸 ——
         * 所以用 MCU 对齐后的 dec_w/dec_h（见 mcuAlignedSize 的说明）。
         * 裁剪仍然按逻辑尺寸 info.width/info.height 算（裁剪框一定能落在画面内）。 */
        srm_config.in.pic_w = dec_w;
        srm_config.in.pic_h = dec_h;
        /* 输入取（裁掉左右补边后的）源画面，等比缩放到 dst_w x dst_h 后落在
         * 输出画面的 (dst_x,dst_y)。0° 档的 crop_* 可能小于整幅；
         * 旋转档恒为整幅（用户明确那条路保持原样）。 */
        srm_config.in.block_w = crop_w;
        srm_config.in.block_h = crop_h;
        srm_config.in.block_offset_x = crop_x;
        srm_config.in.block_offset_y = crop_y;
        srm_config.in.srm_cm = VIDEO_PPA_COLOR_MODE;

        srm_config.out.buffer = lcd_buffer;
        srm_config.out.buffer_size = ALIGN_UP(DISPLAY_WIDTH * DISPLAY_HEIGHT * VIDEO_BYTES_PER_PIXEL, self->data_cache_line_size);
        srm_config.out.pic_w = DISPLAY_WIDTH;
        srm_config.out.pic_h = DISPLAY_HEIGHT;
        /* 目标块的偏移就是黑边的位置。外圈（黑边）由上面那次一次性 memset 负责，
         *    PPA 这里只写画面那一条。 */
        srm_config.out.block_offset_x = dst_x;
        srm_config.out.block_offset_y = dst_y;
        srm_config.out.srm_cm = VIDEO_PPA_COLOR_MODE;

        /* 由自动轮换决定（原来是写死 ANGLE_0）。 */
        srm_config.rotation_angle = (vid_rot == 0)   ? PPA_SRM_ROTATION_ANGLE_0
                                    : (vid_rot == 90) ? PPA_SRM_ROTATION_ANGLE_90
                                    : PPA_SRM_ROTATION_ANGLE_270;
        srm_config.scale_x = scale_x;
        srm_config.scale_y = scale_y;
        srm_config.mirror_x = 0;
        srm_config.mirror_y = 0;
        srm_config.rgb_swap = 0;
        srm_config.byte_swap = 0;
        srm_config.mode = PPA_TRANS_MODE_BLOCKING;

        esp_err_t ret = ppa_do_scale_rotate_mirror(self->ppa_srm_handle, &srm_config);
        if (ret != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "PPA scale failed: %s", esp_err_to_name(ret));
            return;
        }
        ret = esp_lv_adapter_dummy_draw_blit(
                  self->display,
                  0,
                  0,
                  DISPLAY_WIDTH,
                  DISPLAY_HEIGHT,
                  lcd_buffer,
                  false
              );
        if (ret != ESP_OK) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Dummy draw blit failed: %s", esp_err_to_name(ret));
            return;
        }
        self->current_buf_idx = next_buf_idx;

    }

    /* ========================================================================
     * 音频投递 —— 独立任务 + 环形槽位
     * 根因、判据、以及"为什么必须是独立任务"见 VideoPlayer.hpp 里那段注释。
     * ====================================================================== */

    void VideoPlayer::audioTask(void *arg)
    {
        VideoPlayer *self = static_cast<VideoPlayer *>(arg);


        for (;;) {
            AudioSlot s = {};
            if (xQueueReceive(self->audio_ready_q, &s, pdMS_TO_TICKS(50)) != pdTRUE) {
                if (self->audio_task_stop) {
                    break;
                }
                continue;
            }
            /* 收尾：已经取出来的这槽还回去，**不再写**（codec 马上就要被关了） */
            if (self->audio_task_stop) {
                xQueueSend(self->audio_free_q, &s.buf, 0);
                break;
            }

            const int pending = static_cast<int>(uxQueueMessagesWaiting(self->audio_ready_q));
            if (pending > self->ap_slot_peak) {
                self->ap_slot_peak = pending;   /* 抖动余量：队列里还剩几槽 */
            }


            size_t bytes_written = 0;
            esp_err_t ret = bsp_extra_i2s_write(s.buf, s.len, &bytes_written, 1000);
            self->ap_chunks++;
            self->ap_bytes += static_cast<int64_t>(bytes_written);
            if (ret != ESP_OK || bytes_written != s.len) {
                ESP_LOGW(ESP_UTILS_LOG_TAG, "Audio write failed: %s, %u/%u bytes",
                         esp_err_to_name(ret),
                         static_cast<unsigned>(bytes_written),
                         static_cast<unsigned>(s.len));
            }
            /* **专用淡出缓冲不能还回空闲队列** —— 还回去它就变成普通槽，
             * 下次 `pushFadeOut()` 又会抢不到（那就退回到修不好的老样子了）。 */
            if (s.buf != self->fade_buf) {
                xQueueSend(self->audio_free_q, &s.buf, 0);
            }
        }

        /* 置空句柄 = "我停好了"的唯一凭据（stopAudioTask 轮询它）。
         * 不要再借 task notification —— 显示桥也在用同一个原语，任务级通知会被别人取走。 */
        self->audio_task_handle = nullptr;
        vTaskDelete(NULL);
    }

    bool VideoPlayer::startAudioTask()
    {
        if (audio_task_handle) {
            return true;        /* "网格 ↔ 播放"来回切，只起一次 */
        }
        if (!audio_free_q) {
            audio_free_q = xQueueCreate(AUDIO_SLOTS, sizeof(uint8_t *));
        }
        if (!audio_ready_q) {
            audio_ready_q = xQueueCreate(AUDIO_SLOTS, sizeof(AudioSlot));
        }
        if (!audio_free_q || !audio_ready_q) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Create audio queues failed");
            return false;
        }

        /* 槽位一次性分配在 PSRAM；任何一格失败就整块回滚，不留半套队列 */
        for (int i = 0; i < AUDIO_SLOTS; i++) {
            uint8_t *p = static_cast<uint8_t *>(
                             heap_caps_malloc(AUDIO_SLOT_BYTES, MALLOC_CAP_SPIRAM));
            if (p == nullptr) {
                ESP_LOGE(ESP_UTILS_LOG_TAG, "Alloc audio slot %d failed", i);
                uint8_t *q = nullptr;
                while (xQueueReceive(audio_free_q, &q, 0) == pdTRUE) {
                    heap_caps_free(q);
                }
                vQueueDelete(audio_free_q);  audio_free_q = nullptr;
                vQueueDelete(audio_ready_q); audio_ready_q = nullptr;
                return false;
            }
            xQueueSend(audio_free_q, &p, 0);
        }

        /* **专用淡出缓冲**：只给 `pushFadeOut()` 用，**永远不进空闲队列** ——
         * 这样它就不会和生产者抢槽（那正是"换片那一声修不掉"的原因，见 hpp 里的说明）。 */
        fade_buf = static_cast<uint8_t *>(
                       heap_caps_malloc(AUDIO_SLOT_BYTES, MALLOC_CAP_SPIRAM));
        if (fade_buf == nullptr) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "No memory for fade-out buffer; clip-end fade disabled");
        }

        audio_task_stop = false;
        /* 优先级 6 / core 0（avi_player 是 7 / core 0）：低一级无所谓 ——
         * 这个任务**绝大部分时间阻塞**在队列或 DMA 上、不占 CPU；
         * 而视频任务那一帧里真正烧 CPU 的只有 JPEG 解码（约 3 ms），
         * 远小于 DMA 的 32.65 ms。 */
        if (xTaskCreatePinnedToCore(audioTask, "vp_audio", 4096, this, 6,
                                    &audio_task_handle, 0) != pdPASS) {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Create audio task failed");
            audio_task_handle = nullptr;
            return false;
        }
        ESP_LOGI(ESP_UTILS_LOG_TAG, "Audio feeder started (%d slots x %u B)",
                 AUDIO_SLOTS, static_cast<unsigned>(AUDIO_SLOT_BYTES));
        return true;
    }

    bool VideoPlayer::stopAudioTask(TickType_t timeout)
    {
        audio_task_stop = true;
        const TickType_t step = pdMS_TO_TICKS(10);
        for (TickType_t waited = 0; audio_task_handle && waited < timeout; waited += step) {
            vTaskDelay(step);
        }
        if (audio_task_handle) {
            /* 没停干净就**别去动队列** —— 那个任务还在用，释放了就是用后释放。
             * 宁可记一笔，也不要在收尾路径上埋一个随时会炸的野指针。 */
            ESP_LOGE(ESP_UTILS_LOG_TAG, "Audio feeder did not stop in time");
            return false;
        }
        uint8_t *p = nullptr;
        while (audio_free_q && xQueueReceive(audio_free_q, &p, 0) == pdTRUE) {
            if (p != fade_buf) {        /* 专用淡出缓冲不在这条队列里 —— 防御一下，别二次释放 */
                heap_caps_free(p);
            }
        }
        AudioSlot s = {};
        while (audio_ready_q && xQueueReceive(audio_ready_q, &s, 0) == pdTRUE) {
            if (s.buf != fade_buf) {    /* 还没被取走的那一段淡出：跳过，下面单独释放 */
                heap_caps_free(s.buf);
            }
        }
        if (fade_buf) { heap_caps_free(fade_buf); fade_buf = nullptr; }
        if (audio_free_q)  { vQueueDelete(audio_free_q);  audio_free_q  = nullptr; }
        if (audio_ready_q) { vQueueDelete(audio_ready_q); audio_ready_q = nullptr; }

        /* 整程统计。第一判据：`bytes` 应当 == 该片 AVI 里音频 chunk 的字节总和
         * （01.avi = 17,637,320）—— 少一个字节就是丢过音频。
         * 原来的 `waits` 已删（50 ms 超时计数，粒度比被测对象还粗）；
         * "喂送空转"的账现在只在一处：`C9C: [P=] <片名> 收尾 … idle_n/idle_max/idle_sum`。 */
        ESP_LOGI(ESP_UTILS_LOG_TAG,
                 "[AUDIO] chunks=%u dropped=%u bytes=%lld slot_peak=%d",
                 (unsigned)ap_chunks, (unsigned)ap_dropped,
                 (long long)ap_bytes, ap_slot_peak);
        return true;
    }

    /* **收尾淡出**：往"就绪队列"尾部补一段从 `last_l/last_r` 平滑降到 0 的斜坡。
     * 为什么必须是"从上次那个值"起：管子里的内容会连续播下去，斜坡接在它后面 ⇒
     * **斜坡第一帧 = 内容最后一帧**，中间没有台阶；而斜坡尾部落在 0 上（后面本来就是静音）
     * ⇒ **两道台阶一起消失**（这正是"换片/中途滑走那一声咔"的来源，见 hpp 里那段）。 */
    void VideoPlayer::pushFadeOut()
    {
        if (fade_out_pushed) {
            return;     /* 每片只铺一次（自然播完 + 滑动退出可能挨得很近） */
        }
        if (!audio_ready || !audio_ready_q || !fade_buf) {
            fade_fail++;            /* 不留静默失败 */
            return;
        }
        const int spf = (fade_smp_pf > 0) ? fade_smp_pf : 2;
        fade_out_pushed = true;

        int16_t *s = reinterpret_cast<int16_t *>(fade_buf);
        const int l0 = last_l, r0 = last_r;
        for (int i = 0; i < FADE_OUT_FRAMES; i++) {
            const int num = FADE_OUT_FRAMES - i;          /* FADE_OUT_FRAMES … 1 */
            const int16_t gl = static_cast<int16_t>(l0 * num / FADE_OUT_FRAMES);
            const int16_t gr = static_cast<int16_t>(r0 * num / FADE_OUT_FRAMES);
            if (spf >= 2) {
                s[i * spf]     = gl;
                s[i * spf + 1] = gr;
                for (int c = 2; c < spf; c++) {
                    s[i * spf + c] = gl;                  /* 多于 2ch：保守跟随左声道 */
                }
            } else {
                s[i] = gl;
            }
        }

        AudioSlot slot = { fade_buf, static_cast<size_t>(FADE_OUT_FRAMES) * static_cast<size_t>(spf) * 2u };
        /* **这里必须等一小会儿**，不能 0 超时就走人。
         * 第一版就是 0 超时直接放弃 —— 而那正是**换片**那条路失败的原因：
         * 换片那一刻生产者刚倾泻完最后几块，就绪队列可能正好是满的；
         * 而**播放中滑走**时生产者早停了、槽位宽裕，所以那条路第一次就成了。
         * 这是收尾路径（跑在 avi_player 任务上，这一片马上结束），等 20 ms 可以接受。 */
        if (xQueueSend(audio_ready_q, &slot, pdMS_TO_TICKS(20)) != pdTRUE) {
            fade_fail++;
            ESP_LOGW(ESP_UTILS_LOG_TAG, "fade-out not queued (ready queue full) -- that click will stay");
            return;
        }
        fade_ok++;
    }

    void VideoPlayer::audioCallback(frame_data_t *data, void *arg)
    {
        VideoPlayer *self = static_cast<VideoPlayer *>(arg);
        if (!self || !self->audio_ready || !self->loop_playback ||
                !self->audio_ready_q || !self->audio_free_q ||
                !data || data->type != FRAME_TYPE_AUDIO || !data->data || data->data_bytes == 0) {
            return;
        }

        if (data->data_bytes > self->max_chunk_seen) {
            self->max_chunk_seen = data->data_bytes;   /* 盯"最大 chunk vs 读缓冲"的余量 */
        }

        /* **只拷贝、不写。** 这一句返回得多快，就是"视频任务会不会被音频拖住"的全部答案
         * —— 旧实现里它在这里阻塞最多 46 ms，而同一个任务还欠着 11~12 ms 的 PPA。 */

        /* 超长 chunk 实测不存在（最大 16384），真出现就**退化成同步写**：
         * 宁可在这一片里慢一拍，也不许静默丢音频。 */
        if (data->data_bytes > AUDIO_SLOT_BYTES) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "Oversized audio chunk %u B; writing inline",
                     static_cast<unsigned>(data->data_bytes));
            size_t w = 0;
            (void)bsp_extra_i2s_write(data->data, data->data_bytes, &w, 1000);
            self->ap_chunks++;
            self->ap_bytes += static_cast<int64_t>(w);
            return;
        }

        uint8_t *p = nullptr;
        if (xQueueReceive(self->audio_free_q, &p, 0) != pdTRUE) {
            self->ap_dropped++;     /* 抢不到空闲槽 ⇒ 这一坨被丢了（正常应恒为 0） */
            return;
        }
        memcpy(p, data->data, data->data_bytes);

        /* **换片防爆音**：新片开头 23 ms 从 0 起坡（理由见 VideoPlayer.hpp 里那段）。
         * 台阶 → 斜坡 ⇒ 跳变消失 ⇒ 那声"咔"没了。
         * 增益**按"帧"走**（同帧的两个声道同一个增益），否则会破坏立体声像。
         * 坡可能跨 chunk，所以状态是成员、每帧递减。 */
        if (self->fade_in_left > 0 && self->fade_smp_pf > 0) {
            const int spf  = self->fade_smp_pf;
            const int nsmp = static_cast<int>(data->data_bytes / 2);   /* 16 bit ⇒ 2 B/样点 */
            const int nfr  = nsmp / spf;
            int16_t  *sm   = reinterpret_cast<int16_t *>(p);
            for (int i = 0; i < nfr && self->fade_in_left > 0; i++) {
                const int g = FADE_IN_FRAMES - self->fade_in_left;      /* 0 … FADE_IN_FRAMES-1 */
                for (int c = 0; c < spf; c++) {
                    int16_t *v = sm + i * spf + c;
                    *v = static_cast<int16_t>(static_cast<int>(*v) * g / FADE_IN_FRAMES);
                }
                self->fade_in_left--;
            }
        }

        /* 记住"最后写出去的那一帧" —— 收尾淡出**必须从这个值起坡**才接得上
         * （起点接不上就等于又造了一道台阶，那就白做了）。 */
        {
            const int spf2 = (self->fade_smp_pf > 0) ? self->fade_smp_pf : 2;
            const int nfr2 = static_cast<int>(data->data_bytes) / (spf2 * 2);
            if (nfr2 > 0) {
                const int16_t *lastf = reinterpret_cast<const int16_t *>(p) + (nfr2 - 1) * spf2;
                self->last_l = lastf[0];
                self->last_r = (spf2 >= 2) ? lastf[1] : lastf[0];
            }
        }

        AudioSlot s = { p, data->data_bytes };
        if (xQueueSend(self->audio_ready_q, &s, 0) != pdTRUE) {
            xQueueSend(self->audio_free_q, &p, 0);
            self->ap_dropped++;
        }
    }

    void VideoPlayer::audioSetClockCallback(uint32_t rate, uint32_t bits, uint32_t ch, void *arg)
    {
        VideoPlayer *self = static_cast<VideoPlayer *>(arg);
        if (!self || !self->audio_ready || !self->loop_playback) {
            return;
        }

        const uint32_t sample_rate = rate ? rate : CODEC_DEFAULT_SAMPLE_RATE;
        const uint32_t bit_width = bits ? bits : CODEC_DEFAULT_BIT_WIDTH;
        const i2s_slot_mode_t mode = (ch <= 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

        /* **新片子的头 23 ms 起坡**（防爆音）。
         * 这里正好是天然的"一片开始"分界：`avi_player` 每开一个片子都会回调一次这里
         * （HEADER 阶段，**在本片第一个音频 chunk 之前**）⇒ 每片都重新武装。
         * 只在 16 bit 时启用；非 16 bit 就置 0（不做会算错的坡，也不假装做了）。 */
        if (bit_width == 16 && ch >= 1) {
            self->fade_smp_pf  = static_cast<int>(ch);
            self->fade_in_left = FADE_IN_FRAMES;
        } else {
            self->fade_in_left = 0;
        }
        /* 新片子的**收尾**记账清零 —— 上一片的 `last_*` 和"已铺过"标记都不能带过来。
         * （上一片播完时铺的那一段已经进了队列，不受这里影响。） */
        self->fade_out_pushed = false;
        self->last_l = 0;
        self->last_r = 0;

        /* 按片结算"音频喂送"统计。`avi_player` 每开一个片都会回调一次这里
         * （avi_player.c:142-148），正好是天然的"一片"分界。
         * 判据：`bytes` 应当 == 该片 AVI 里音频 chunk 字节总和（可离线核对）。 */
        if (self->ap_chunks > 0) {
            /* `fade=ok/fail`：**上一片收尾时那一段淡出到底铺成了没有**。
             * `fail>0` ⇒ 那一声咔必然还在，而且不是"淡出没用"，是**它根本没被放进去**。 */
            ESP_LOGI(ESP_UTILS_LOG_TAG,
                     "[AUDIO] prev clip: chunks=%u dropped=%u bytes=%lld slot_peak=%d max_chunk=%u fade=%u/%u",
                     (unsigned)self->ap_chunks, (unsigned)self->ap_dropped,
                     (long long)self->ap_bytes, self->ap_slot_peak,
                     (unsigned)self->max_chunk_seen,
                     (unsigned)self->fade_ok, (unsigned)self->fade_fail);
        }
        /* **读缓冲余量告警**：`read_frame` 装不下 chunk 时**不是静静截断** —— 它返回 0，
         * 于是解析从错误的位置继续、整段播放坏掉。所以在"还能放"的时候就先叫一声，
         * 好让我们去调 `AVI_READ_BUFFER_BYTES`，而不是等它崩了才发现。 */
        if (self->max_chunk_seen * 2 >= AVI_READ_BUFFER_BYTES) {
            ESP_LOGW(ESP_UTILS_LOG_TAG,
                     "AVI chunk %u B is close to the %u B read buffer; raise AVI_READ_BUFFER_BYTES",
                     (unsigned)self->max_chunk_seen, (unsigned)AVI_READ_BUFFER_BYTES);
        }
        self->ap_chunks = 0; self->ap_dropped = 0;
        self->ap_bytes = 0; self->ap_slot_peak = 0;
        self->max_chunk_seen = 0;
        self->fade_ok = 0; self->fade_fail = 0;

        /* ---- 什么时候**不**该去重配 codec ------------------------------------
         * `bsp_extra_codec_set_fs()` 不是"改个采样率"那么轻：它会把播放通路
         * **关掉再打开**（esp_codec_dev_close → open → 解除静音），并且让
         * I2S TX 通道经历一次 disable/enable。而 `avi_player` **每开一个片子**
         * 都会走到这里（avi_player.c:142-148，在 HEADER 状态里）
         * ⇒ 不判重的话，"换片"这个动作天然带一声咔哒。
         * 实测：卡上四段素材（含一段**完全无音轨**的）**每道接缝都有那一声**，
         * 无音轨那片也响 ⇒ 说明它与音频数据无关，就是这次重配。
         *
         * 两种情况可以直接跳过：
         *   ① `ch == 0` —— 这个片子根本没有音轨。
         * 此时 rate/bits/ch 是**上一个文件残留的值**（AVI 解析器不会清零），
         *      所以这里必须判 ch==0，不能拿那三个数当依据。
         *   ② 参数与**当前已生效的**格式完全一致 —— 再配一遍纯属白拆一次。
         *
         * 参数真的变了则照常重配。本工程四段素材都是 44100/16/2
         * ⇒ 正常情况下**只有第一次**会真正配下去。 */
        if (ch == 0 || (self->audio_fs_valid && self->audio_fs_rate == sample_rate &&
                        self->audio_fs_bits == bit_width && self->audio_fs_ch == ch)) {
            return;
        }

        esp_err_t ret = bsp_extra_codec_set_fs(sample_rate, bit_width, mode);
        if (ret != ESP_OK) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "Audio clock setup failed: %s", esp_err_to_name(ret));
        } else {
            /* `bsp_extra_codec_set_fs()` 内部是 **close → open → 解除静音**（见上面那段），
             * 重新打开 codec 会把硬件音量寄存器复位 ⇒ 这里必须**再补一次全局音量**，
             * 且补的是用户在 Settings 里调的值，不是写死的 80（否则"开一个视频 = 音量被拉回 80"）。
             * 放在"真正重配成功"之后 —— 跳过重配的分支什么都不用做（寄存器没被碰过）。 */
            (void)bsp_extra_codec_volume_set(bsp_extra_codec_volume_get(), nullptr);
            self->audio_fs_valid = true;
            self->audio_fs_rate = sample_rate;
            self->audio_fs_bits = bit_width;
            self->audio_fs_ch = ch;
            ESP_LOGI(ESP_UTILS_LOG_TAG, "Audio format applied (%u Hz/%u bit/%u ch)",
                     static_cast<unsigned>(sample_rate),
                     static_cast<unsigned>(bit_width),
                     static_cast<unsigned>(ch));
        }
    }

    void VideoPlayer::playEndCallback(void *arg)
    {
        VideoPlayer *self = static_cast<VideoPlayer *>(arg);
        /* **自然播完也先淡出**：管子里的尾巴播完那一刻，就是从波形中途硬切到 0 ——
         * 那正是"换片那一声咔"。斜坡补在尾巴后面，顺带把换片那 ~30 ms 空档也盖住了。 */
        self->pushFadeOut();
        self->is_playing = false;
    }


    VideoPlayer::TouchAction VideoPlayer::pollTouch()
    {
        if (!touch_handle) {
            return TouchAction::None;
        }

        esp_lcd_touch_read_data(touch_handle);

        esp_lcd_touch_point_data_t points[1] = {};
        uint8_t point_num = 0;
        const esp_err_t ret = esp_lcd_touch_get_data(touch_handle, points, &point_num, 1);

        if (ret != ESP_OK || point_num == 0) {
            /* 手指抬起来了 —— **此刻**才结算：
             *   全程没越过阈值 ⇒ 一次"点击"
             *   越过了          ⇒ 一次"滑动"（回上一级 / 预览网格）
             * 原来是"一越过阈值就立刻下结论"，那样在手离开之前结论就定了，
             *    永远等不到"点一下"。⇒ 改成抬起时才结算。
             * 位移要用**最后看到的位置**算：快速一划有可能在下一次轮询（30 ms）之前
             *    就抬手了，那一拍 point_num 已经是 0；只看"抬手时拿不到点"会把它误判成点击。
             * 一次点击后**记下时刻**，若 350 ms 内又来一次点击 ⇒ 判为双击（切旋转）。
             *    单击本身**不再有任何动作** —— 播放中的出口只剩"滑动回网格"这一条
             *    （与改动前一致：`SWIPE_EXIT_THRESHOLD` 这个名字有点误导，
             *      它只是"算不算滑动"的阈值，从来没决定过"滑到哪去"）。 */
            if (!touch_active) {
                return TouchAction::None;
            }
            const int dx = abs(static_cast<int>(touch_last_x) - static_cast<int>(touch_start_x));
            const int dy = abs(static_cast<int>(touch_last_y) - static_cast<int>(touch_start_y));
            const bool moved = touch_moved ||
                               dx > SWIPE_EXIT_THRESHOLD || dy > SWIPE_EXIT_THRESHOLD;
            touch_active = false;
            touch_moved = false;
            if (moved) {
                last_tap_us = 0;        /* 划过就不参与双击判定 */
                return TouchAction::Swipe;
            }
            const int64_t now_us = esp_timer_get_time();
            if (last_tap_us != 0 && (now_us - last_tap_us) <= DOUBLE_TAP_WINDOW_US) {
                last_tap_us = 0;        /* 双击结算完就清掉，免得"三击"又被算成第二次双击 */
                return TouchAction::DoubleTap;
            }
            last_tap_us = now_us;
            return TouchAction::Tap;
        }

        if (!touch_active) {
            touch_active = true;
            touch_moved = false;
            touch_start_x = points[0].x;
            touch_start_y = points[0].y;
            touch_last_x = points[0].x;
            touch_last_y = points[0].y;
            return TouchAction::None;
        }

        touch_last_x = points[0].x;
        touch_last_y = points[0].y;

        const int dx = abs(static_cast<int>(touch_last_x) - static_cast<int>(touch_start_x));
        const int dy = abs(static_cast<int>(touch_last_y) - static_cast<int>(touch_start_y));
        if (dx > SWIPE_EXIT_THRESHOLD || dy > SWIPE_EXIT_THRESHOLD) {
            touch_moved = true;     /* 只记录；真正的结算等抬手（见上） */
        }
        return TouchAction::None;
    }

    void VideoPlayer::playTask(void *arg)
    {
        VideoPlayer *self = static_cast<VideoPlayer *>(arg);
        bool request_close = false;
        /* 每一次 run() 都从"codec 还没配过"这个状态开始 —— 否则上一轮留下的
         * 记忆会让本轮第一次开片跳过重配（见 audioSetClockCallback 的说明）。 */
        self->audio_fs_valid = false;
        self->audio_ready = (bsp_extra_codec_init() == ESP_OK);
        if (self->audio_ready) {
            /* **用"当前的全局音量"，不要写死 `CODEC_DEFAULT_VOLUME`。**
             * `bsp_extra_codec_init()` 之内会走 `bsp_extra_codec_set_fs()` → `esp_codec_dev_open()`，
             * 那是**重新打开 codec**，硬件音量寄存器会被复位成芯片默认值 ⇒ 这里必须补一次。
             * 但补的值要是用户在 Settings 里调的那个（`bsp_extra_codec_volume_get()` 就是它），
             * 否则每开一个视频都会把全局音量**拉回 80%**（用户 2026-10-20 实测报的 bug）。 */
            const int volume = bsp_extra_codec_volume_get();
            esp_err_t volume_ret = bsp_extra_codec_volume_set(volume, nullptr);
            if (volume_ret != ESP_OK) {
                ESP_LOGW(ESP_UTILS_LOG_TAG, "Set video volume failed: %s", esp_err_to_name(volume_ret));
            }
        } else {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "Audio codec unavailable; continue video-only playback");
        }

        /* 起"音频喂送"任务。失败就退化成"只有画面"，
         * 而不是让 avi_player 收到一个会阻塞的 audio_cb。 */
        if (self->audio_ready && !self->startAudioTask()) {
            ESP_LOGW(ESP_UTILS_LOG_TAG, "Audio feeder unavailable; continue video-only playback");
            self->audio_ready = false;
        }


        /* **这个值原来是 2 MB —— 它只买到了一件事：换片时白读 2 MB。**
         * `avi_player` 的 HEADER 状态是这么解头的（avi_player.c:131）：
         *     *BytesRD = fread(pbuffer, buffer_size, 1, file);   ← 整个 buffer 读进来
         *     avi_parser(...);
         *     fseek(file, movi_start, SEEK_SET);                 ← 然后立刻 seek 回去
         * 也就是说**那 2 MB 读完就扔**，而它唯一真正的用途是 `read_frame` 的容量上限
         * （`if (length < head.size)` 就报错）。库自己的默认值是 **20 KB**（avi_player.c:383）。
         * 实测：2 MB ÷ ~9.5 MB/s ≈ **221 ms** —— 与日志里量到的换片断口严丝合缝。
         * 那 221 ms 里没有音频 ⇒ I2S 的 `auto_clear` 补零 ⇒ 输出从信号**断到零**、
         * 再从零**跳回信号** ⇒ **两道台阶 = 换片那一声"咔"**（且不 100% 出现，见 §36）。
         * ⇒ 降到 128 KB：读头 ≈ 13 ms，加上换片其余开销 ≈ **30 ms < 管子 65 ms** ⇒ 管子不流空。
         * 下限由"最大 chunk"决定（实测全卡最大 51 KB，见 hpp 里的说明）。
         * **又调回了 256 KB**：高清素材（720×1280/q6）单帧最大 192 KB > 128 KB，
         * `read_frame` 会直接返回 0 ⇒ 停播。读头 256 KB ≈ **27 ms**，仍远小于 DMA 98 ms。 */
        avi_player_config_t cfg = {
            .buffer_size = AVI_READ_BUFFER_BYTES,
            .video_cb = videoCallback,
            .audio_cb = self->audio_ready ? audioCallback : nullptr,
            .audio_set_clock_cb = self->audio_ready ? audioSetClockCallback : nullptr,
            .avi_play_end_cb = playEndCallback,
            .priority = 7,
            .coreID = 0,
            .user_data = self,
            /* 同样是实测定值：该任务实测峰值 **2,548 B**，
             * 8192 = 3.2 倍余量。它在 PSRAM（下面 `stack_in_psram`），
             * 所以这一项省的是 PSRAM 不是内部 RAM —— 属于顺带收干净，不是必需。 */
            .stack_size = AVI_PLAYER_TASK_STACK,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
            .stack_in_psram = true,
#endif
        };

        if (avi_player_init(cfg, &self->avi_player_handle) != ESP_OK)
        {
            ESP_LOGE(ESP_UTILS_LOG_TAG, "avi_player_init failed");
            /* 这条提前 return 必须自己收音频任务 —— 它不会走到下面的收尾段，
             * 漏了就是"任务 + 4×32 KB PSRAM"整套泄漏（而且接着还要关 codec）。 */
            (void)self->stopAudioTask(pdMS_TO_TICKS(1000));
            (void)bsp_extra_codec_output_stop();
            self->audio_ready = false;
            self->play_task_handle = nullptr;
            vTaskDelete(NULL);
            return;
        }

        /* **绝对不要在这里**再调 initDisplayBypass() —— 它内部那句
         * `esp_lv_adapter_pause(-1)` 会把 LVGL 整个停掉，而下面状态①要建 LVGL 网格
         * （`bsp_extra_display_lock(-1)` 是**永久等**）⇒ 当场死锁：
         * 屏幕全黑、滑动无效、也不重启（是阻塞不是崩溃）。
         * 第一版就是这么挂的，日志停在
         * `Video display bypass: 800x1280, 3 frame buffers` 之后再无输出。
         * **旁路的开/关已经挪进下面状态②里**，与网格严格互斥。 */

        /* ====================================================================
         * 两个状态来回切（这是本 app 的主循环）：
         *   ① **网格**：LVGL 活着、显示旁路关着 —— 等用户点一个视频
         *   ② **播放**：`initDisplayBypass()` 接管屏幕，它内部的
         *      `esp_lv_adapter_pause()` 会把 LVGL（连同它的触摸 indev）整个停掉，
         *      改由我们自己那个 touch_handle 轮询（滑动 = 回网格；双击 = 转画面）
         *
         * 两者天然互斥 ⇒ **触摸永远不会被两处同时消费**。
         * （`esp_lcd_touch_read_data()` 会把控制器状态读走；网格上 LVGL 必须独占它，
         *   否则点击会集体失效 —— 这就是网格上不自己读触摸、只用 LVGL 事件的原因。）
         * ================================================================== */
        while (self->loop_playback)
        {
            /* ---------------- ① 网格 ---------------- */
            self->grid_pick = -1;
            self->showSelectGrid();
            while (self->loop_playback && self->grid_pick < 0) {
                /* 一拍补一张缩略图（补完立刻返回，就是空转）。
                 * 这里是**唯一**驱动缩略图的地方 —— 没有 lv_timer（理由见
                 * applyThumbToCell() 的说明：框架会把 run() 期间建的定时器再删一遍）。 */
                self->fillNextThumb();
                vTaskDelay(pdMS_TO_TICKS(60));
            }
            self->destroySelectGrid();

            /* 网格上**只有两个出口**，而且都由框架发起：
             *   · 下方手势条上滑 → 框架 `processNavigationEvent(HOME)` → `pause()`
             *   · 屏幕左/右边缘横滑 → 框架 `BACK` → `back()` → `notifyCoreClosed()`
             * 两条路最终都会让 `loop_playback` 变 false（`pause()`/`close()` 都会调
             * `stopPlaybackTask()`）⇒ 这里只判断它，**不要**自己再判断"用户是不是想退出"。
             * （原先那个 `grid_exit` 已删掉：同一次滑动会被框架和我们的 LVGL 事件
             *   同时命中，两边都动手正是"黑屏有背光"的来源。） */
            if (!self->loop_playback) {
                break;
            }
            const int start_index = self->grid_pick;

            /* ---------------- ② 播放 ---------------- */
            if (self->initDisplayBypass() != ESP_OK) {
                ESP_LOGE(ESP_UTILS_LOG_TAG, "initDisplayBypass failed");
                request_close = true;       /* 宁可干净地关掉，也不要留一块死屏 */
                break;
            }
            self->back_to_grid = false;
            self->touch_active = false;
            self->touch_moved = false;

            /* 从用户点的那个开始往后连播（绕回来）——
             * 用户要的是"**能选**"，不是"只能放一个"（2026-10-16 拍板）。 */
            for (int n = 0;
                    n < self->avi_file_count && self->loop_playback && !self->back_to_grid;
                    n++)
            {
                const int i = (start_index + n) % self->avi_file_count;

                while (self->is_paused && self->loop_playback && !self->back_to_grid)
                {
                    /* 只有滑动才是出口。双击在这里**故意不接** —— 这条循环是 100 ms 一拍，
                     * 而一次点击只有 50~150 ms，很可能**整个按下-抬起都被跳过**，
                     * 接了也是个时灵时不灵的入口；何况 is_paused 时屏幕根本不在看。 */
                    if (self->pollTouch() == TouchAction::Swipe) {
                        self->back_to_grid = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                if (!self->loop_playback || self->back_to_grid)
                    break;

                ESP_LOGI(ESP_UTILS_LOG_TAG, "Playing AVI file: %s", self->avi_file_list[i]);
                self->avi_stop_requested = false;     /* 新片子：stop 的记账清零 */
                /* 补边检测按**片子**重来（连播时同一个 display bypass 会换片）。
                 * `last_video_crop_*` **故意不重置**：如果上一部是放大的竖屏片、这一部
                 * 是满幅片，新片第一帧的 crop=0 与上一版不同 ⇒ 自动触发整屏清黑。 */
                self->pb_left = 0;
                self->pb_width = 0;
                self->pb_cand_left = 0;
                self->pb_cand_w = 0;
                self->pb_agree = 0;
                self->pb_attempts = 0;
                self->pb_done = false;
                self->is_playing = true;
                if (avi_player_play_from_file(self->avi_player_handle, self->avi_file_list[i]) != ESP_OK) {
                    ESP_LOGE(ESP_UTILS_LOG_TAG, "Failed to play file: %s", self->avi_file_list[i]);
                    self->is_playing = false;
                    continue;
                }

                while (self->is_playing && self->loop_playback && !self->back_to_grid)
                {
                    /* 播放中的手势（最终版）：
                     *   **滑动** ⇒ 回预览网格（"退一级"）
                     *   **双击** ⇒ 切视频画面旋转（0° → 90° → 270° → 0°）
                     *   **单击** ⇒ 什么都不做
                     * 单击必须"什么都不做"：一次双击里必然含一次单击，单击若还留着出口，
                     * 双击会先把人踢回网格、旋转永远轮不到。
                     *
                     * 为什么播放中不交给框架的手势条：`initDisplayBypass()` 里那句
                     * `esp_lv_adapter_pause()` 把 **LVGL 连同它的 indev 一起停了**，
                     * 而框架手势是靠一个 lv_timer 读 indev 的 ⇒ 播放中它本来就收不到触摸。
                     * 所以这里必须自己轮询（网格上则相反：LVGL 活着，交给框架）。
                     * 两级出口正好接上：播放 →（滑动）→ 网格 →（手势条）→ 桌面。
                     * （用户 2026-10-16 明确："一律仅回退上一级"。） */
                    const TouchAction act = self->pollTouch();
                    if (act == TouchAction::Swipe) {
                        /* **先铺淡出、再请求停止**（顺序不能反）：
                         * 反过来的话 `play_stop` 已经切了，斜坡就来不及接在那道断口前面。
                         * 这一句就是"播放中途滑回网格也有一声咔"的解法（用户实测过那一声）。 */
                        self->pushFadeOut();
                        self->back_to_grid = true;
                        self->avi_stop_requested = true;
                        avi_player_play_stop(self->avi_player_handle);
                        self->is_playing = false;
                        break;
                    }
                    if (act == TouchAction::DoubleTap) {
                        /* 只置数、不在这里改 `video_rot_idx` —— 真正的切换在 videoCallback 里
                         * 做（那一侧才是唯一写者，也就不必在这里加锁）。
                         * 写成 `x = x + 1` 而不是 `x++`：`rot_requests` 是 volatile，
                         * C++20 起对 volatile 用 `++` 是 deprecated 警告（-Wvolatile）。 */
                        self->rot_requests = self->rot_requests + 1;
                    }

                    if (self->is_paused)
                    {
                        self->avi_stop_requested = true;
                        avi_player_play_stop(self->avi_player_handle);
                        self->is_playing = false;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }

            /* 这里**不要**加"等迟到的帧"的延迟 —— 试过 150 ms，结果把退出流程拖过了
             * 框架 2 秒的耐心（实测 `Timed out waiting for playback task` → `Pause failed`）。
             * 迟到的帧现在由 videoCallback 里的 `!self->display` 判据兜住，不需要靠等待。 */
            self->deinitDisplayBypass();

            if (!self->loop_playback) {
                break;
            }
            /* 回网格之前让显示这层落一下地（旁路刚还回去、LVGL 刚 resume） */
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        /* **收尾时不要再 stop 一次。**
         * `avi_player_play_stop()` 只是把事件位置起来，真正干活的是任务里的 END 分支
         * `fclose()`（avi_player.c:219，而且**不把 file 置空**）。
         * 如果收尾这次与循环里那次挨得太近，任务会把 STOP 消费两遍
         * ⇒ **第二次 fclose** ⇒ Load access fault 重启（实测栈里有 `_fclose_r`）。
         * 原版这里隔着一个 `vTaskDelay(500)` 天然错开 —— 我改结构时把它去掉了，这个坑是自己挖的。
         * ⇒ 现在用 avi_stop_requested 记账，**并且无论如何都留一段时间让任务把 STOP 处理完**。 */
        if (!self->avi_stop_requested) {
            avi_player_play_stop(self->avi_player_handle);
            self->avi_stop_requested = true;
        }
        /* 这里也**不要**加延迟（试过 200 ms，和上面那 150 ms 一起把退出拖超时）。
         * 不需要它的理由：现在同一轮里 STOP 只会被请求一次（上面那个记账），
         * 而 STOP 和 DEINIT 都只是事件位 —— 任务在同一个 `xEventGroupWaitBits` 里
         * 按固定顺序先处理 STOP 再处理 DEINIT，天然不会重复走 END 分支。 */
        avi_player_deinit(self->avi_player_handle);
        self->avi_player_handle = nullptr;
        /* **先把喂音频的任务停干净，再关 codec。**
         * 反过来就是"让一个还活着的任务往一个正在被拆掉的 codec 里写" ——
         * 那正是"迟到的东西踩到已拆资源"那一类（停止请求必须记账，多请求一次就会走两遍结束流程）。
         * 预算：任务最多阻塞在一次 I2S 写（≤46 ms）或一拍空等（50 ms）里，
         * 所以 1000 ms 足够；而宿主框架给 pause() 的耐心是 2 s。 */
        (void)self->stopAudioTask(pdMS_TO_TICKS(1000));
        (void)bsp_extra_codec_output_stop();
        self->audio_ready = false;
        self->deinitDisplayBypass();

        /* 置空 `play_task_handle` 就是"我停好了"的唯一凭据 ——
         * `stopPlaybackTask()` 现在轮询它（**不再用 task notification**，
         * 理由见那个函数的注释）。所以这一句必须放在**拆完显示之后**。 */
        self->play_task_handle = nullptr;

        if (request_close)
        {
            self->notifyCoreClosed();
        }

        vTaskDelete(NULL);
    }

} // namespace esp_brookesia::apps
