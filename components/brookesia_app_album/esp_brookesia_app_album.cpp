/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:App:Album"
#include "esp_lib_utils.h"

#include "esp_brookesia_app_album.hpp"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

/* WebP 走软件解码（板上没有硬件 WebP 解码器，ESP 组件仓库也没有官方组件）。
 * libwebp 的编译宏（尤其是 WEBP_SWAP_16BIT_CSP=1）在 components/libwebp/CMakeLists.txt 里。 */
#include "webp/decode.h"

/* 只为了拿 BSP_SD_MOUNT_POINT（SD 卡的挂载点）。分区由 main.cpp 在开机时挂好；
 * 这里刻意用 BSP 的宏而不是自己写死 "/sdcard"，免得哪天挂载点改了这里跟着漂。
 * 相应地 CMakeLists 的 REQUIRES 里要加 esp32_p4_wifi6_touch_lcd_x。
 *
 * 壁纸这条路径还要 bsp_extra_display_lock()：壁纸 worker 不在 LVGL 任务上，
 * 而桌面样式是框架的全局状态 ⇒ 改它之前必须拿同一把显示锁。
 * 这把锁必须可重入 —— 框架有时是持着锁调进我们的回调的，不可重入就会当场自锁死。 */
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

/* 壁纸要拿"主屏对象"（框架自己的壁纸也是设在它上面）。 */
#include "systems/base/esp_brookesia_base_context.hpp"
#include "systems/base/esp_brookesia_base_display.hpp"

using namespace esp_brookesia::systems;

#define APP_NAME            "Album"

/* SD 卡根目录。**只扫根、不递归** —— 用户放素材的习惯就是直接丢在卡根。 */
static constexpr const char *SD_DIR = BSP_SD_MOUNT_POINT;

/* 大图页倍率滑动条的**定位基准**（距右边缘多远）。
 * 它现在**不再**是"从视口里扣掉的一条" —— 视口已铺满整屏，滑动条浮在图片上方。 */
static constexpr int ZOOM_BAR_W = 80;

/*
 * 输出的 RGB565 字节序：JPEG 库给的 LE，LVGL 在小端机器上也按 LE 解释。
 * 万一实机颜色不对（红蓝互换 / 明显偏色），把这里改成 JPEG_PIXEL_FORMAT_RGB565_BE 再试。
 */
#define ALBUM_JPEG_PIXEL_FMT    JPEG_PIXEL_FORMAT_RGB565_LE

/*
 * esp_new_jpeg 的 scale 输出实际上必须对齐到 **16**，不是文档里写的 8。
 *
 * 用户 2026-10-13 实测（第一张图 175%~179% 撕裂：马赛克 + 绿边 + 画面错乱）：
 *     源 1440×1440，解到 **1392 → 正常**，解到 **1400 → 撕裂**。
 *     1392 = 16×87；1400 = 16×87.5（是 8 的倍数，不是 16 的）。
 *   同一批里所有正常尺寸（800=16×50、1280=16×80、1440=16×90）全是 16 的倍数，
 *   而 1392 不是 32 的倍数却正常 ⇒ 对齐单位就是 **16**，不多不少。
 *
 * 头文件 esp_jpeg_dec.h:31 只写了 "Scale.height and scale.width require integer
 *    multiples of 8" —— **文档说少了**，按 8 对齐会送进不合法的尺寸。
 *    教科书式的"合法输入出坏结果"：库不报错，直接画花。
 */
static constexpr int JPEG_ALIGN = 16;

/* 向下对齐到 JPEG_ALIGN，且不小于 JPEG_ALIGN。 */
static inline int _alignJpeg(int v)
{
    const int a = (v / JPEG_ALIGN) * JPEG_ALIGN;
    return (a < JPEG_ALIGN) ? JPEG_ALIGN : a;
}

/*
 * esp_new_jpeg 在"目标输出尺寸**太接近源尺寸**"时会**静默截断**：
 *    返回 JPEG_ERR_OK、jpeg_dec_get_outbuf_len() 也照报满，但只写了一个**前缀**，
 *    后面的行**根本没写** —— 那里还是内存垃圾，屏幕上就是用户看到的
 *    "马赛克 + 绿边 + 撕裂"（垃圾字节 0xAA 按 RGB565 解出来正是灰绿色）。
 *
 * 2026-10-13 在板子上实测确认（开机的 albumSelftestJpegScale 探针）：
 *   - 源 1440×1440：输出 ≥1400 全坏、≤1392 全对（坏带 = 差 8~40 像素）
 *   - 源 1280×1280：输出 1272 坏、≤1264 全对（坏带 = 差 8 像素）
 *   - **确定性**：同一尺寸跑两遍，未写字节数逐字节相同
 *   - 与输入在 **flash 还是 PSRAM 无关**（先拷进 PSRAM 再解，结果一样）
 *   - 与 **RGB565 / RGB888 无关**（未写*字节*数不同，但未写*占比*都是 42.1%
 *     ⇒ 它是按"**行**"截断的，不是按字节）
 *   - 官方示例 test_app/main/test_decoder.c 的调用姿势与我们的**完全一致**
 *     ⇒ 是库的缺陷，不是我们少做了哪个步骤（inbuf/parse_header/outbuf/out_size/process）
 *
 * ⇒ 对策：**永不请求源长边附近的尺寸**。留 1/16（6.25%）余量 ——
 *    实测最宽的坏带是 3.3%（1440 源差 48），正好 2 倍余量；再用 64 px 的绝对下限兜底
 *    （源较小时按比例算出来的余量太薄）。
 *    超过这个上限的倍率**不算作废**：改用 LVGL 缩放把安全尺寸吹上去（见 _syncDetail），
 *    于是滑动条全程都能拖，只有最上面一小段会略微变软 —— 总好过整块花屏。
 *    `_decodeTo()` 里另有一道**哨兵守卫**做安全网：万一日后遇上本文没覆盖到的尺寸，
 *    也会丢掉半张图而不是把它画出去。
 */
static constexpr int JPEG_SAFE_GAP_RATIO = 16;   /* 余量 = 源长边 / 16 */
static constexpr int JPEG_SAFE_GAP_MIN   = 64;   /* 余量的绝对下限（像素） */

/* 给定源图长边，返回**可以安全请求**的最大缩放输出尺寸。 */
static inline int _safeEdge(int src_long)
{
    int gap = src_long / JPEG_SAFE_GAP_RATIO;
    if (gap < JPEG_SAFE_GAP_MIN) {
        gap = JPEG_SAFE_GAP_MIN;
    }
    const int safe = src_long - gap;
    return (safe >= JPEG_ALIGN) ? safe : JPEG_ALIGN;
}

/*
 * 内存预算。
 *
 * 为什么必须有：网页上传的是**手机原图**（4000 px 级）。200% 时"1:1 显示"要一张
 * 2560×1600 的位图 = 8.2 MB；再加上解码缓冲，峰值能吃到十几 MB PSRAM。
 * 用户实测就撞上了 —— 表现是"某张图放大后卡在缓冲期出不来"（分配一直失败 +
 * 每 250ms 重试一次）和"快速切换长图有概率重启"（分配失败后 LVGL 自己的缓冲
 * 也拿不到内存）。
 *
 * 所以：**单个位图的像素数封顶**。超出的倍率**照常能放大**，只是那一段由 LVGL
 * 缩放出图（略软、平移略慢）—— 稳定性优先。这是用户明确认可过的取舍。
 * 调这个值就能在"清晰/顺滑"和"省内存"之间挪；串口会打出实时 PSRAM 余量，照它调。
 */
static constexpr int64_t MAX_DETAIL_PX = 3600000;   /* ≈ 6.9 MB @RGB565 */

/*
 * WebP 解码的**内部**内存预算。
 * 这一项和 MAX_DETAIL_PX 不是一回事 —— 后者管的是"我们自己的输出缓冲"，
 * 而 libwebp 自己的临时缓冲不受它控制：
 *   无损（VP8L）：**固定**分配 w*h*4（src/dec/vp8l_dec.c 的 AllocateInternalBuffers32b，
 *                 即使我们只要一张缩小后的输出，它也照样按源图尺寸分配）；
 *   有损 + alpha：额外 w*h 的 alpha 平面（src/dec/frame_dec.c 的 AllocateMemory）。
 * 12 MB 是给相机（2×2,048,000 B **连续** PSRAM）和相册大图留出余量后的上限；
 * 超过就明确拒绝、把原因写在格子上，不硬解 —— 否则表现是"点开就失败"，最难查。
 */
static constexpr int64_t WEBP_INTERNAL_BUDGET = 12 * 1024 * 1024;

/* 给定源尺寸与目标长边，算出解码器**实际**会输出的宽高。
 * 必须与 _decodeTo() 里那段算式一致 —— 所以那边也转调本函数，从根上杜绝两边漂移。
 * 用途：按**真实**尺寸分配缓冲，而不是按"长边²" —— 后者对非正方形图浪费好几倍内存
 * （16:9 的图浪费 5.6 倍），上面那次内存吃紧正是从这儿来的。 */
static void _expectedOutSize(int src_w, int src_h, int target_edge, int &out_w, int &out_h)
{
    const int longest = (src_w > src_h) ? src_w : src_h;
    if (longest > 0 && target_edge < longest) {
        const float s = (float)target_edge / (float)longest;
        out_w = _alignJpeg((int)((float)src_w * s));
        out_h = _alignJpeg((int)((float)src_h * s));
    } else {
        out_w = src_w;
        out_h = src_h;
    }
}

/* 在内存预算内，把目标长边压到"解出来装得下"的最大值。
 * 面积按缩放比的**平方**走，所以由目标面积反推线性比值再乘长边。 */
static int _budgetEdge(int src_w, int src_h, int target)
{
    const int64_t src_px = (int64_t)src_w * (int64_t)src_h;
    const int longest = (src_w > src_h) ? src_w : src_h;
    if (src_px <= 0 || longest <= 0 || src_px <= MAX_DETAIL_PX) {
        return target;      /* 源图本身就不超预算 */
    }
    const double k = sqrt((double)MAX_DETAIL_PX / (double)src_px);
    int cap = (int)((double)longest * k);
    cap = (cap / JPEG_ALIGN) * JPEG_ALIGN;
    if (cap < JPEG_ALIGN) {
        cap = JPEG_ALIGN;
    }
    return (target < cap) ? target : cap;
}

/*
 * WebP 专用解码 —— 解成 RGB565、写进调用方的外置缓冲、按 target_edge 等比缩放。
 *
 * 输出尺寸**必须**与调用方的 `_expectedOutSize()` 完全一致：
 *    `_ensureBase/_ensureDetail/_fillNextThumb` 都按那个尺寸分配缓冲。
 *    这里刻意沿用同一套 16 像素对齐（WebP 本身不要求），为的是让两边只有**一份**算法。
 *
 * RGB565 的字节序由 libwebp 的编译宏 `WEBP_SWAP_16BIT_CSP=1` 保证
 *    （见 components/libwebp/CMakeLists.txt）。不加的话它写 [rg, gb]（大端），
 *    LVGL 按小端解释 ⇒ 红绿蓝全错，而且看起来像"文件坏了"。
 *
 * target_edge >= 源长边时**不放大**（和 JPEG 路径一致）；放大由上层 _ensureScaled 做，
 * 这样平移才能走 LVGL 的 memcpy 快路径。
 */
static bool _decodeWebpBytes(const uint8_t *data, size_t len, int target_edge,
                             uint8_t *buf, size_t buf_size,
                             int &out_w, int &out_h, int &out_len, const char *name)
{
    WebPBitstreamFeatures feat;
    if (WebPGetFeatures(data, len, &feat) != VP8_STATUS_OK) {
        ESP_UTILS_LOGE("'%s': WebP 头解析失败", name);
        return false;
    }
    if (feat.has_animation) {
        /* 相册按**首帧**显示；不拦，但要说清 —— 否则"图不动"会被当成 bug。 */
        ESP_UTILS_LOGW("'%s': 动画 WebP（%dx%d），按首帧显示", name, feat.width, feat.height);
    }

    /*
     * libwebp 的**内部**缓冲不受我们的 MAX_DETAIL_PX 控制：
     *   无损 VP8L 固定 w*h*4；有损 + alpha 额外 w*h。见 WEBP_INTERNAL_BUDGET。
     */
    const int64_t internal = (feat.format == 2)
                             ? (int64_t)feat.width * feat.height * 4
                             : (feat.has_alpha ? (int64_t)feat.width * feat.height : 0);
    if (internal > WEBP_INTERNAL_BUDGET) {
        ESP_UTILS_LOGE("'%s': WebP 内部缓冲需要 %lld MB（预算 %lld MB），拒绝解码",
                       name, (long long)(internal / (1024 * 1024)),
                       (long long)(WEBP_INTERNAL_BUDGET / (1024 * 1024)));
        return false;
    }

    const int longest = (feat.width > feat.height) ? feat.width : feat.height;
    _expectedOutSize(feat.width, feat.height, target_edge, out_w, out_h);
    const size_t need = (size_t)out_w * (size_t)out_h * 2u;
    if (need > buf_size) {
        ESP_UTILS_LOGE("'%s': webp 需要 %u B，但缓冲只有 %u B",
                       name, (unsigned)need, (unsigned)buf_size);
        return false;
    }

    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) {
        ESP_UTILS_LOGE("'%s': WebPInitDecoderConfig 失败", name);
        return false;
    }
    cfg.output.colorspace         = MODE_RGB_565;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba        = buf;
    cfg.output.u.RGBA.stride      = out_w * 2;
    cfg.output.u.RGBA.size        = need;
    if (target_edge < longest) {
        cfg.options.use_scaling   = 1;
        cfg.options.scaled_width  = out_w;
        cfg.options.scaled_height = out_h;
    }

    const int64_t t0 = esp_timer_get_time();
    const VP8StatusCode st = WebPDecode(data, len, &cfg);
    if (st != VP8_STATUS_OK) {
        ESP_UTILS_LOGE("'%s': WebPDecode 失败（%d）", name, (int)st);
        return false;
    }

    out_len = (int)need;
    ESP_UTILS_LOGI("'%s': WebP %s %dx%d -> %dx%d，解码 %lld ms",
                   name, (feat.format == 2) ? "lossless" : "lossy",
                   feat.width, feat.height, out_w, out_h,
                   (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/*
 * 双线性放大：把 sw×sh 的 RGB565 位图（行跨距 sstride 字节）放大到 dw×dh，写进紧凑的 dst。
 *
 * 只在**一种情况**下用：屏幕上想要的大小超过了"解码器敢给"的上限（见 JPEG_SAFE_GAP_*）。
 * 存在的**唯一理由**是把这点缩放**一次性做掉**，好让之后的平移回到 LVGL 的 memcpy 快路径 ——
 * 若把这张小一点的位图交给 LVGL 每帧去放大，拖动会明显变卡（用户实测报过）。
 *
 * 用双线性而不是最近邻：放大倍率只有几个百分点时两者看不出差别，
 * 但**源图本来就比屏幕小**的情况下倍率可能到 2 倍，最近邻会看出明显方块。
 */
static bool _upscaleBilinear(const uint8_t *src, int sw, int sh, int sstride,
                             uint8_t *dst, int dw, int dh)
{
    if (sw < 2 || sh < 2 || dw <= 0 || dh <= 0) {
        ESP_UTILS_LOGE("放大参数非法：%dx%d -> %dx%d", sw, sh, dw, dh);
        return false;
    }

    /* 水平方向的源坐标/权重对每一行都一样 —— 先算一次，省掉内层循环里的除法 */
    int *map = (int *)heap_caps_malloc(sizeof(int) * (size_t)dw * 2u,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (map == nullptr) {
        /* 以前这里是静默 return —— 调用方以为放大成功，拿到的是一块**没写过**的缓冲。
         *    做壁纸时这条会直接变成"桌面上一片花屏"，所以必须能报出来。 */
        ESP_UTILS_LOGE("放大映射表 %u B 分配失败（PSRAM free %u KB）",
                       (unsigned)(sizeof(int) * (size_t)dw * 2u),
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }
    int *mx = map;
    int *mw = map + dw;
    for (int x = 0; x < dw; x++) {
        /* 16.16 定点，按**像素中心**对齐：src_x = (x + 0.5) * sw / dw - 0.5 */
        const int64_t fx = ((int64_t)(2 * x + 1) * sw * 65536) / (2 * dw) - 32768;
        int i = (int)(fx >> 16);
        int f = (int)(fx & 0xFFFF);
        if (i < 0) {
            i = 0;
            f = 0;
        }
        if (i > sw - 2) {
            i = sw - 2;
            f = 65535;
        }
        mx[x] = i;
        mw[x] = f;
    }

    for (int y = 0; y < dh; y++) {
        const int64_t fy = ((int64_t)(2 * y + 1) * sh * 65536) / (2 * dh) - 32768;
        int j = (int)(fy >> 16);
        int g = (int)(fy & 0xFFFF);
        if (j < 0) {
            j = 0;
            g = 0;
        }
        if (j > sh - 2) {
            j = sh - 2;
            g = 65535;
        }

        const uint16_t *r0 = (const uint16_t *)(src + (size_t)j * (size_t)sstride);
        const uint16_t *r1 = (const uint16_t *)(src + (size_t)(j + 1) * (size_t)sstride);
        uint16_t *drow = (uint16_t *)(dst + (size_t)y * (size_t)dw * 2u);

        const int gy = 65536 - g;
        for (int x = 0; x < dw; x++) {
            const int i  = mx[x];
            const int gx = 65536 - mw[x];
            /* 四个权重之和恰好 65536：最后一项用减法凑出来，免得舍入让总和不为 1
             * （那会让整体亮度漂移）。 */
            const int w00 = (int)(((int64_t)gx * gy) >> 16);
            const int w01 = (int)(((int64_t)mw[x] * gy) >> 16);
            const int w10 = (int)(((int64_t)gx * g) >> 16);
            const int w11 = 65536 - w00 - w01 - w10;

            const uint16_t a = r0[i];
            const uint16_t b = r0[i + 1];
            const uint16_t c = r1[i];
            const uint16_t d = r1[i + 1];

            const int rr = (((a >> 11) & 0x1F) * w00 + ((b >> 11) & 0x1F) * w01 +
                            ((c >> 11) & 0x1F) * w10 + ((d >> 11) & 0x1F) * w11) >> 16;
            const int gg = (((a >>  5) & 0x3F) * w00 + ((b >>  5) & 0x3F) * w01 +
                            ((c >>  5) & 0x3F) * w10 + ((d >>  5) & 0x3F) * w11) >> 16;
            const int bb = ((a & 0x1F) * w00 + (b & 0x1F) * w01 +
                            (c & 0x1F) * w10 + (d & 0x1F) * w11) >> 16;

            drow[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
        }
    }

    heap_caps_free(map);
    return true;
}

/* 嵌进固件的图片，符号由 CMake 的 target_add_binary_data 生成（只取文件名） */
extern "C" {
extern const uint8_t _binary_photo1_jpg_start[];
extern const uint8_t _binary_photo1_jpg_end[];
extern const uint8_t _binary_photo2_jpg_start[];
extern const uint8_t _binary_photo2_jpg_end[];
}

/* 桌面图标（112x112 ARGB8888），见 assets/esp_brookesia_image_large_app_album_112_112.c */
extern const lv_image_dsc_t esp_brookesia_image_large_app_album_112_112;

namespace esp_brookesia::apps {

Album *Album::_instance = nullptr;
AlbumWallpaperJob Album::_wp_job {};

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

/**
 * 直接从 JPEG 的 SOF 段读出原始宽高。
 * 为什么要自己读：jpeg_dec_open() 时就得把 scale（目标分辨率）告诉解码器，
 * 但那时还不知道原图多大 —— 先用十几行把尺寸读出来，就不用"开→关→再开"。
 * sof（可传 nullptr）回传 SOF 标记：0xC0 = 基线式（能解），0xC2 = 渐进式（**板上解不了**）。
 */
static bool _jpegInfo(const uint8_t *d, size_t n, int &w, int &h, uint8_t *sof = nullptr)
{
    size_t i = 2;   /* 跳过 SOI(FFD8) */

    while (i + 9 < n) {
        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        const uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;
            continue;
        }
        const uint16_t seg = (uint16_t)((d[i + 2] << 8) | d[i + 3]);
        /* SOF0..SOF15，排除 DHT(C4) / JPG(C8) / DAC(CC) */
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            h = (d[i + 5] << 8) | d[i + 6];
            w = (d[i + 7] << 8) | d[i + 8];
            if (sof != nullptr) {
                *sof = m;
            }
            return (w > 0 && h > 0);
        }
        i += 2 + seg;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* 文件名 / 格式识别 —— 目的只有一个：把"解不了"说成**具体原因**，      */
/* 而不是让用户看到"传进去了但相册里没有"，那种沉默最难查。             */
/* ------------------------------------------------------------------ */

static bool _endsWithNoCase(const char *s, const char *suffix)
{
    const size_t ls = strlen(s);
    const size_t lf = strlen(suffix);
    if (ls < lf) {
        return false;
    }
    for (size_t i = 0; i < lf; i++) {
        char a = s[ls - lf + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* 我们**能解**的：板上只有 esp_new_jpeg，且只支持基线式 JPEG；PNG/WebP 都没有解码器
 * （libpng 虽然被链接进固件，但全工程没有任何地方调用它）。 */
static bool _isJpegName(const char *name)
{
    return _endsWithNoCase(name, ".jpg") || _endsWithNoCase(name, ".jpeg");
}

/* 看起来像图片的（**含我们解不了**的格式）。
 * 为什么要放宽：只按"能解"过滤的话，用户传个 .png 相册里会**什么都不显示**，
 * 看起来就是"传丢了"。先列出来，再把真实格式写在格子上，用户才知道该转成什么。
 * （早期就是被"静默少一张"坑过的。） */
static bool _looksLikeImage(const char *name)
{
    static const char *kExt[] = { ".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp",
                                  ".webp", ".tif", ".tiff", ".heic", ".heif", ".avif" };
    for (size_t i = 0; i < sizeof(kExt) / sizeof(kExt[0]); i++) {
        if (_endsWithNoCase(name, kExt[i])) {
            return true;
        }
    }
    return false;
}

/*
 * 显示用的短名：超过 limit 个字符就截断、末尾补 ".."。
 * 为什么用 ASCII 的两个点而不是 "…"：LVGL 内置的 montserrat 字体不含 U+2026，
 * 画出来是个方框（不是省略号）。
 * limit 由 Album::_nameLimit() 给 —— 用户要求"全部以图片1的名称长度为基准"，
 * 这样所有格子的文件名长度一致，不会一张长一张短。
 */
static void _shortName(const char *name, int limit, char *out, size_t out_sz)
{
    if (name == nullptr || out_sz == 0) {
        return;
    }
    if (limit < 4) {
        limit = 4;      /* 再短连"X.."都放不下，没意义 */
    }
    if ((int)strlen(name) <= limit) {
        snprintf(out, out_sz, "%s", name);
        return;
    }
    snprintf(out, out_sz, "%.*s..", limit - 2, name);
}

/* 认一认这堆字节**到底是什么格式**（按内容，不按扩展名）。认不出来返回 nullptr。 */
static const char *_sniffFormat(const uint8_t *d, size_t n)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return "JPEG";
    }
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        return "PNG";
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        return "WebP";
    }
    if (n >= 6 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
        return "GIF";
    }
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') {
        return "BMP";
    }
    if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 0x2A) ||
                   (d[0] == 'M' && d[1] == 'M' && d[3] == 0x2A))) {
        return "TIFF";
    }
    if (n >= 12 && memcmp(d + 4, "ftyp", 4) == 0) {
        if (memcmp(d + 8, "heic", 4) == 0 || memcmp(d + 8, "heix", 4) == 0 ||
            memcmp(d + 8, "hevc", 4) == 0 || memcmp(d + 8, "mif1", 4) == 0) {
            return "HEIC";
        }
        if (memcmp(d + 8, "avif", 4) == 0) {
            return "AVIF";
        }
    }
    return nullptr;
}

/* 把 SOF 标记翻译成人话。esp_new_jpeg 只支持基线式，遇到别的就在**解析头部**时失败，
 * 那时如果只打一句"解析失败"，根本分不清是文件坏了还是格式不支持。 */
/* 这里返回的字符串**必须全 ASCII** —— 它们最终会画在占位格上，而那条 label 用的是
 *    lv_font_montserrat_16，**它一个中文字形都没有**。
 *    写成中文的后果用户 2026-10-14 实测过：格子上一片乱码，等于什么都没说 ——
 *    而这条提示存在的全部意义就是"解不了的时候说清楚为什么"。
 *    （同源判据这个文件里早就有了：_shortName() 用 ASCII 的 ".." 而不是 "…"，
 *      因为 montserrat 不含 U+2026，画出来是个方框。）
 *    SOF 编号照旧保留：用户要靠它知道该把这个文件转成什么。 */
static const char *_sofName(uint8_t m)
{
    switch (m) {
    case 0xC0: return "SOF0 baseline";
    case 0xC1: return "SOF1 ext-seq";
    case 0xC2: return "SOF2 progressive";
    case 0xC3: return "SOF3 lossless";
    case 0xC5: return "SOF5";
    case 0xC6: return "SOF6";
    case 0xC7: return "SOF7";
    case 0xC9: return "SOF9 arith";
    case 0xCA: return "SOF10 prog-arith";
    case 0xCB: return "SOF11";
    case 0xCD: return "SOF13";
    case 0xCE: return "SOF14";
    case 0xCF: return "SOF15";
    default:   return "non-baseline JPEG";
    }
}

/*
 * 从 WebP 文件的 RIFF 块头里拿尺寸与格式标志。
 *
 * 为什么不用 `WebPGetInfo()` 直接把前 4 KB 丢进去：WebP 允许在图像块前面放
 * ICCP / EXIF / XMP 等块，而它们可能**大于 4 KB** —— 那时 `WebPGetInfo` 只会回
 * `NOT_ENOUGH_DATA`，并不能说明文件坏了。这里按每个块的 size **seek** 下一个块，
 * 所以不管前面的块多大都只读几十个字节。
 *
 * 输出：
 *   fmt   = 1（有损 VP8）/ 2（无损 VP8L）/ 0（没找到图像块，只拿到 VP8X 画布尺寸）
 *   alpha / anim —— VP8X 的 flags（bit4 / bit1）或 VP8L 头里的 alpha 位。
 *
 * 三种头的尺寸位置（都在块 payload 的最前面）：
 *   VP8X : payload[4..6] = 画布宽-1（24-bit LE），payload[7..9] = 高-1
 *   VP8  : payload[6..7] = 宽（14-bit），payload[8..9] = 高（14-bit）
 *   VP8L : payload[0]=0x2F，payload[1..4] 的 bit0-13 = 宽-1、bit14-27 = 高-1
 */
static bool _webpInfoFile(const char *path, int &w, int &h,
                          uint8_t &fmt, bool &alpha, bool &anim)
{
    w = 0;
    h = 0;
    fmt = 0;
    alpha = false;
    anim = false;

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }

    uint8_t riff[12];
    bool ok = false;
    if (fread(riff, 1, sizeof(riff), fp) == sizeof(riff) &&
            memcmp(riff, "RIFF", 4) == 0 && memcmp(riff + 8, "WEBP", 4) == 0) {
        /* 只走有限个块，防止坏文件里出现环 */
        for (int n = 0; n < 32; n++) {
            uint8_t ch[8];
            if (fread(ch, 1, sizeof(ch), fp) != sizeof(ch)) {
                break;
            }
            const uint32_t csize = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                                   ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
            const long payload = ftell(fp);

            if (memcmp(ch, "VP8X", 4) == 0 && csize >= 10) {
                uint8_t p[10];
                if (fread(p, 1, sizeof(p), fp) != sizeof(p)) {
                    break;
                }
                w = 1 + (int)((uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16));
                h = 1 + (int)((uint32_t)p[7] | ((uint32_t)p[8] << 8) | ((uint32_t)p[9] << 16));
                alpha = (p[0] & 0x10) != 0;
                anim  = (p[0] & 0x02) != 0;
                ok = (w > 0 && h > 0);
                /* 还要继续往后找 VP8/VP8L，才能知道有损/无损 */
            } else if (memcmp(ch, "VP8 ", 4) == 0 && csize >= 10) {
                uint8_t p[10];
                if (fread(p, 1, sizeof(p), fp) != sizeof(p)) {
                    break;
                }
                /* 必须看到帧起始码 9D 01 2A，否则不是合法的 VP8 关键帧头 */
                if (p[3] == 0x9d && p[4] == 0x01 && p[5] == 0x2a) {
                    w = (int)((p[6] | (p[7] << 8)) & 0x3fff);
                    h = (int)((p[8] | (p[9] << 8)) & 0x3fff);
                    fmt = 1;
                    ok = (w > 0 && h > 0);
                }
                break;
            } else if (memcmp(ch, "VP8L", 4) == 0 && csize >= 5) {
                uint8_t p[5];
                if (fread(p, 1, sizeof(p), fp) != sizeof(p)) {
                    break;
                }
                if (p[0] == 0x2f) {
                    const uint32_t bits = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                                          ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
                    w = 1 + (int)(bits & 0x3fff);
                    h = 1 + (int)((bits >> 14) & 0x3fff);
                    alpha = ((bits >> 28) & 1u) != 0;
                    fmt = 2;
                    ok = (w > 0 && h > 0);
                }
                break;
            }

            /* 跳到下一个块；RIFF 的块按偶数字节对齐 */
            if (csize > 0x7fffffffu) {
                break;
            }
            if (fseek(fp, payload + (long)csize + (long)(csize & 1u), SEEK_SET) != 0) {
                break;
            }
        }
    }

    fclose(fp);
    return ok;
}

/* 把整个文件读进 PSRAM（16 字节对齐）。调用方负责 heap_caps_free()。 */
static uint8_t *_readFileAlloc(const char *path, size_t *out_len)
{
    *out_len = 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return nullptr;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return nullptr;
    }

    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(16, (size_t)st.st_size,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        ESP_UTILS_LOGE("psram alloc %ld B failed for '%s'", (long)st.st_size, path);
        fclose(fp);
        return nullptr;
    }

    /*
     * stdio 的默认 FILE 缓冲只有百来字节（CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0 的后果，
     *    这就是每次只读几百字节的原因）⇒ 一个几 MB 的图会变成**上万次**小读，SD 上就是 1.8 MB/s。
     *    这里给 64 KB 的 PSRAM 缓冲。实测这条能把"读+解"里的读盘部分砍掉一大半，
     *    而 WebP 的解码本身就有 0.5~1.2 s，读盘再慢就是用户能感觉到的"点开卡一下"。
     */
    void *iobuf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (iobuf != nullptr) {
        setvbuf(fp, (char *)iobuf, _IOFBF, 64 * 1024);
    }

    const size_t got = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (iobuf != nullptr) {
        heap_caps_free(iobuf);
    }
    if (got != (size_t)st.st_size) {
        ESP_UTILS_LOGE("short read on '%s' (%u/%ld)", path, (unsigned)got, (long)st.st_size);
        heap_caps_free(buf);
        return nullptr;
    }

    *out_len = got;
    return buf;
}


/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

Album *Album::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new Album(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

/*
 * 桌面图标用我们自己的 112x112 图（原来是 nullptr 走框架默认图标，会和别的默认图标撞脸）。
 * use_default_screen = true ⇒ 我们在 lv_scr_act() 上画，退出时框架自动回收。
 * use_navigation_bar 保持 false —— 我们有自己的顶栏；系统本来就有快捷方式隐藏导航栏，
 *   这里再要一条纯属多余（用户指出的）。
 */
Album::Album(bool use_status_bar, bool use_navigation_bar) :
    App(APP_NAME, &esp_brookesia_image_large_app_album_112_112, true, use_status_bar, use_navigation_bar)
{
}

Album::~Album()
{
    ESP_UTILS_LOGD("Destroy(@0x%p)", this);

    for (int i = 0; i < _photo_count; i++) {
        if (_photos[i].pixels != nullptr) {
            heap_caps_free(_photos[i].pixels);
            _photos[i].pixels = nullptr;
        }
        if (_photos[i].grid_px != nullptr) {
            heap_caps_free(_photos[i].grid_px);
            _photos[i].grid_px = nullptr;
        }
    }

    _releaseBigBuffers();

    /*
     * 这里**不要**再 lv_timer_del(_zoom_timer/_thumb_timer)。
     * 它们是在 run() 里建的 ⇒ 归框架的记录表管 ⇒ app 关闭时框架已经删过一遍了
     * （见 run() 开头那段长注释）。析构时那两个指针要么已经被 _forgetLvObjects()
     * 清空，要么就是**野指针**，两种情况下 del 都是二次释放。让框架去管即可。
     */
}

bool Album::run(void)
{
    ESP_UTILS_LOGD("Run(@0x%p)", this);

    /*
     * 关键：框架在 app 退出时会**销毁默认屏幕**，所以上一轮建的 lv 对象全是野指针。
     * 不在这里清空的话，第二次进入 app 时 _openFull() 会看到 _fullview != nullptr 而跳过创建，
     * 转而对已释放的 _full_img 调 lv_image_set_src() ⇒ 用后释放 ⇒ 崩溃重启。
     * （解出来的像素在 PSRAM 里、由本类自己持有，不受屏幕销毁影响，不用重解。）
     */
    _forgetLvObjects();

    /*
     * 两个定时器**每次 run() 都重建**，绝不能用"指针非空就复用"来判断。
     *
     * 原因：`App::Config::enable_recycle_resource` **默认是开的**（base_app.hpp:48）。
     * 开了之后，框架会把 **run() 期间创建的屏幕 / 定时器 / 动画全部记录下来**
     * （endRecordResource()，base_app.cpp:155-180），并在 **app 关闭时逐个销毁**
     * （cleanRecordResource()，base_app.cpp:265-291，定时器走 lv_timer_del()）。
     *
     * 「清理后台」就是一次 app 关闭 ⇒ 这两个定时器**已经被框架删掉了**，
     * 而我们的成员指针还指着那块已释放的内存。这里原来写的是
     * `if (_zoom_timer == nullptr) { 创建 }` —— 野指针非空 ⇒ **跳过创建**，
     * 紧接着对野指针调 lv_timer_resume()。后果是两面（2026-10-13 用户 100% 复现）：
     *   ① 定时器再也不跑 ⇒ 开闸的唯一执行者没了 ⇒ 倍率一过 130% 图片就再也拖不动
     *      （≤130% 那一档本就不关闸，所以看起来"只有高倍率坏"，正好对上）；
     *   ② lv_timer_resume() 往已释放的堆块里写 paused/last_run ⇒ 那块内存可能已被
     *      LVGL 复用成 lv_obj_t ⇒ 对象树出现坏链 ⇒ lv_obj_tree_walk 无限递归
     *      ⇒ **栈保护复位**；同一处损坏先被堆管理器撞上就是 TLSF assert。
     *      两次实测崩溃都落在这两条上。
     *
     * _forgetLvObjects() 已经把这两个指针清空（**只清不删**，见那里的说明），
     * 所以这里无条件创建是安全的：run() 只在 app 处于 CLOSED 时被调用
     * （base_manager.cpp:224-253：还活着的走 resume，不走进这里），上一轮的定时器
     * 已经归框架收走了，不会重复。
     *
     * 另外**不要再 lv_timer_resume()** —— 新建出来的定时器本来就是跑的。
     *
     * 记一条通用判据：**凡是在 run() 里建的东西，成员指针都不能跨 app 生命周期用。**
     */
    _zoom_timer  = lv_timer_create(_onZoomSettle, 250, nullptr);
    _thumb_timer = lv_timer_create(_onThumbTick, 50, nullptr);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /*
     * 内嵌那 2 张解码一次就够（app 重进时复用已有的位图）；
     * **上传目录每次都重扫** —— 否则你刚在手机上传的图，不重启板子就永远看不见
     * （音乐播放器当初也是这么处理的）。
     * 重扫很便宜：只读每张图的文件头认尺寸，不解码。
     */
    if (_builtin_count == 0) {
        _loadBuiltins();
        _builtin_count = _photo_count;
    }
    _releaseUploads();
    _scanDir(UPLOAD_DIR);   /* 网页控制台上传的 */
    _scanDir(SD_DIR);       /* SD 卡根目录的（卡由 main.cpp 在开机时挂好） */

    _buildGrid();
    return true;
}

bool Album::back(void)
{
    ESP_UTILS_LOGD("Back(@0x%p)", this);

    /*
     * 退出前**必须**做这两件事，顺序也别换：
     *   ① 停掉两个定时器 —— 它们是跨 app 生命周期常驻的（不是每次 run() 重建），
     *      框架马上要销毁默认屏幕，再让它们跑一拍就会在野指针上操作；
     *   ② 忘掉全部 lv 指针 —— 双保险，万一有哪个退出路径没经过这里，
     *      下一次 run() 也会先清一遍（见 run() 开头）。
     * 这两条防的都是用后释放：踩到代码段是崩溃重启、踩到堆结构是死锁卡死。
     */
    _teardownForScreenLoss();

    /* 与官方 Calculator 一致 */
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");

    return true;
}

/*
 * 框架关 app 时走这里 —— **包括"打开别的 app"这条人人都会走的路**
 * （`App::processClose()` 里 `ESP_UTILS_CHECK_FALSE_GOTO(close(), …)`，
 *   esp_brookesia_base_app.cpp:537，而且它在**销毁屏幕之前**调，所以此刻
 *   `_full_img` 等 lv 指针还都是有效的）。
 *
 * 原来这里没实现 ⇒ 基类的空实现 ⇒ 只有"边缘横滑"那一条路会放大缓冲。
 *    实测：相册看过大图后开着 VideoPlayer，PSRAM free 从 14,527 KB
 *    掉到 2,112 KB 且**一动不动** —— `_detail_buf`(4,489,216) + `_base_buf`(1,515,520)
 *    约 6 MB 一直挂着 ⇒ 视频解不出帧（黑屏有声音）、相册自己的缩略图 `decode failed`。
 *    `free` 和 `largest` **一起掉**才是这个形状（只有 largest 掉是碎片，两者一起掉是泄漏）。
 */
bool Album::close(void)
{
    ESP_UTILS_LOGD("Close(@0x%p)", this);
    _teardownForScreenLoss();
    return true;
}

/*
 * 下方手势条上滑 = 进最近任务 = `pause()`。
 * 与 `close()` 的差别只有一条：**屏幕不会被销毁**（框架只是把它藏起来，
 * `resume` 时原样还回来）⇒ 所以这里**不能** `_forgetLvObjects()`，
 * 那些指针回来还要用；两个定时器也不用停（对象都还有效）。
 *
 * 但 6 MB 的大缓冲**必须放**：它们跟"这个界面还在不在"无关，
 * 只是为"现在正在看的那张大图"服务的。留着它们，后台的相册就会把前台视频饿死。
 */
bool Album::pause(void)
{
    const bool in_full = (_fullview != nullptr) && !lv_obj_has_flag(_fullview, LV_OBJ_FLAG_HIDDEN);
    ESP_UTILS_LOGD("Pause(@0x%p) in_full=%d photo=%d", this, (int)in_full,
                   in_full ? _full_index : -1);

    /*
     * **只在真的停在大图页时记账，不要在这里写 -1。**
     * 实测（2026-10-25）：框架会在 1 秒内**连调两次 pause()**（手势一次、
     * 立刻打开别的 app 又一次）。第一次记下"第 5 张"，第二次进来时 `_closeFull()`
     * 已经把大图页藏起来了 ⇒ `in_full == false` ⇒ 如果这里无条件写 -1，
     * 就把刚记下的索引冲掉 ⇒ 回来时停在网格而不是那张图。
     * 清 -1 的职责只有两处：`resume()`（消费掉）和 `_forgetLvObjects()`（屏幕没了）。
     */
    if (in_full && _photo_count > 0) {
        _resume_full = _full_index;
    }
    /*
     * 直接复用"退出大图页"那段：断引用 + 记账清零 + 放大缓冲 + 退回网格。
     * 退回网格是**故意**的 —— 回来时若重解失败（内存真不够了），
     * 用户看到的应该是"网格"（还能操作），而不是一块空白的黑屏。
     */
    _closeFull();
    ESP_UTILS_LOGI("Pause 完成：大缓冲已释放（PSRAM free %u KB，回来自动重开第 %d 张）",
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                   _resume_full);
    return true;
}

bool Album::resume(void)
{
    const int idx = _resume_full;
    _resume_full = -1;
    ESP_UTILS_LOGD("Resume(@0x%p) restore_full=%d", this, idx);

    /* 只恢复"进来时正停在大图页"这一种情形。网格页什么都是现成的（缩略图一直在），不用管。 */
    if (idx >= 0 && idx < _photo_count && _fullview != nullptr) {
        _openFull(idx);
    }
    return true;
}

void Album::_forgetLvObjects(void)
{
    _grid = nullptr;
    _fullview = nullptr;
    _zoom_view = nullptr;
    _zoom_slider = nullptr;
    _zoom_label = nullptr;
    _full_img = nullptr;
    _full_caption = nullptr;
    _wp_btn = nullptr;
    _wp_label = nullptr;
    _wp_revert_at = 0;
    _full_index = 0;
    for (int i = 0; i < MAX_PHOTOS; i++) {
        _cell_img[i] = nullptr;
        _cell_note[i] = nullptr;
    }
    /* 拖动闸的对象随屏幕一起没了 ⇒ 状态也要归零，否则重进 app 时会以为还关着闸 */
    _pan_gated   = false;
    _pan_started = 0;

    /*
     * 两个定时器**只清指针，不要 lv_timer_del()**。
     * 它们是在 run() 里建的 ⇒ 被框架记录在案 ⇒ app 关闭时框架已经删过了
     * （见 run() 开头那段长注释）。这里再 del 一次就是对已释放内存二次释放。
     * 清空的目的是让下次 run() **无条件重建**，而不是拿"非空"当"已创建"。
     */
    _zoom_timer  = nullptr;
    _thumb_timer = nullptr;

    /* 屏幕都没了，"回到大图页"这件事当然也作废（否则下次 run() 之后一次意外的 resume
     * 会拿旧索引去解一张图 —— 指针已经清空，_openFull 会自己创建对象，不算错，
     * 但"我进关掉过的 app 却直接跳到大图页"是没人想要的）。 */
    _resume_full = -1;
}

/*
 * 大图页那套东西"软收摊"：**只放内存、不碰 lv 对象**。
 * 所以它在两个上下文里都能用：
 *   · `_closeFull()` —— 屏幕还在，只是回网格；
 *   · `pause()`      —— 屏幕还在（被框架藏起来），马上要进后台。
 * 释放顺序不能换：**先断开 `_full_img` 对像素的引用，再 free**（控件还挂着这块像素，先 free 就是用后释放；借出去的指针必须先解绑）。
 */
void Album::_releaseViewer(void)
{
    if (_full_img != nullptr) {
        lv_image_set_src(_full_img, nullptr);
    }
    _detail_edge      = 0;
    _detail_photo     = -1;
    _detail_want      = 0;
    _detail_fail_photo  = -1;
    _detail_fail_target = -1;
    _shown_photo = -1;
    _shown_long  = 0;
    _openPanGate();         /* 离开大图页/进后台，拖动闸没意义了，放掉 */
    _releaseBigBuffers();
}

/*
 * `back()` / `close()` 共用：这两个出口之后，框架**会销毁默认屏幕**
 * （`enable_recycle_resource` 或 `enable_default_screen`），所以除了放内存，
 * 还要停定时器、把 lv 指针全忘掉。
 */
void Album::_teardownForScreenLoss(void)
{
    if (_zoom_timer != nullptr) {
        lv_timer_pause(_zoom_timer);
    }
    if (_thumb_timer != nullptr) {
        lv_timer_pause(_thumb_timer);
    }
    _releaseViewer();
    _forgetLvObjects();
}

/* ------------------------------------------------------------------ */
/* 解码                                                                */
/* ------------------------------------------------------------------ */

/*
 * 内嵌在固件里的那 2 张示例图。**只做一次**（它们在 flash 里，解码要几百毫秒，重进没必要重来）。
 *
 * 加图片前必须确认源图是**基线式（baseline / SOF0）** JPEG！
 * esp_new_jpeg 只支持基线式，**不支持渐进式（progressive / SOF2）**。
 * 渐进式的塞进来会解不出来 —— 所以下面把失败张数也画到屏幕上，不让它悄悄发生。
 * （踩过一次：夜空那张原本是渐进式，结果相册里少一张，还显示成 "1 / 1 photo2"。）
 */
bool Album::_loadBuiltins(void)
{
    struct Src {
        const char    *name;
        const uint8_t *beg;
        const uint8_t *end;
    };
    static const Src kSrcs[] = {
        { "photo1", _binary_photo1_jpg_start, _binary_photo1_jpg_end },
        { "photo2", _binary_photo2_jpg_start, _binary_photo2_jpg_end },
    };

    for (size_t i = 0; i < sizeof(kSrcs) / sizeof(kSrcs[0]) && _photo_count < MAX_PHOTOS; i++) {
        Photo &p = _photos[_photo_count];

        memset(&p, 0, sizeof(p));
        p.name     = kSrcs[i].name;
        p.data     = kSrcs[i].beg;
        p.data_len = (size_t)(kSrcs[i].end - kSrcs[i].beg);
        p.baseline = true;          /* 编译进来的这两张是我们自己确认过的基线式 */
        p.from_file = false;

        if (_decodeInto(p)) {       /* 内嵌图直接就是常驻的基准位图 */
            _photo_count++;
        } else {
            p.grid_note = "decode failed";
            _failed_count++;
            _photo_count++;
            ESP_UTILS_LOGE("decode '%s' failed (%u B)", p.name, (unsigned)p.data_len);
        }
    }

    ESP_UTILS_LOGI("内嵌 %d 张", _photo_count);
    return (_photo_count > 0);
}

/* 放掉上一次扫到的上传条目（内嵌的**保留** —— 它们的位图常驻，重解是浪费）。 */
void Album::_releaseUploads(void)
{
    while (_photo_count > _builtin_count) {
        _photo_count--;
        Photo &p = _photos[_photo_count];
        if (p.pixels != nullptr) {
            heap_caps_free(p.pixels);
        }
        if (p.grid_px != nullptr) {
            heap_caps_free(p.grid_px);
        }
        memset(&p, 0, sizeof(p));
    }
    /* 重新数一遍"画不出来"的张数：内嵌的 + 接下来扫到的 */
    _failed_count = 0;
    for (int i = 0; i < _photo_count; i++) {
        if (_photos[i].grid_note != nullptr) {
            _failed_count++;
        }
    }
    _truncated = 0;
}

/*
 * 扫一个目录里的图片，追加到 _photos 末尾。
 * 调用方会给两个目录：数据分区 /data/upload（网页上传的）和 SD 卡根。
 * 每个目录都**只扫根、不递归** —— 用户放素材的习惯就是直接丢在根目录。
 * 每张**只读文件头**拿尺寸和 SOF 标记 —— 不把几 MB 的原图读进来，也**不解码**。
 * 真正的网格小图由 _fillNextThumb() 一拍一张地补（占位格先顶上，别让界面卡住）。
 */
bool Album::_scanDir(const char *dir)
{
    DIR *dp = opendir(dir);
    if (dp == nullptr) {
        /* 不是错误：SD 卡没插、或者数据分区没挂上 —— 只是少一批素材，界面照常。 */
        ESP_UTILS_LOGW("opendir(%s) 失败（分区没挂上 / 卡没插？）", dir);
        return false;
    }

    /* 这两个是**累计**计数器（_releaseUploads 只把内嵌图那段重算过），
     * 所以要记下进本函数时的基线，日志里报**本次**的增量 —— 否则第二个目录会把
     * 第一个目录的失败数再报一遍，看起来像翻倍。 */
    const int failed_before = _failed_count;
    const int truncated_before = _truncated;

    int added = 0;
    struct dirent *ent = nullptr;

    while ((ent = readdir(dp)) != nullptr) {
        /* 这一行挡掉 "."、".."、隐藏文件，**以及 .thumbs 子目录**
         * （那是上次试做留下的残留，别把它当图片）。 */
        if (ent->d_name[0] == '.') {
            continue;
        }
        /* 用"**看起来像**图片"而不是"我们能解"来过滤 —— 否则用户传个 .png，
         * 相册里**什么都不显示**，看起来就是"传丢了"。先列出来，再把真实格式写在格子上。 */
        if (!_looksLikeImage(ent->d_name)) {
            continue;
        }
        /* 超过上限就**必须告警**，不能悄悄丢 —— 写死数量的常量会在素材变多时静默截断
         * （栽过两次）。 */
        if (_photo_count >= MAX_PHOTOS) {
            _truncated++;
            continue;
        }

        /* 名字长得我们装不下就跳过。上传侧本来就限制在 ALBUM_NAME_MAX 内，
         * 出现更长的说明不是我们放进去的东西。
         * 先落到一个**局部**变量再往结构体里拷：直接拿 p.name_buf 当 snprintf 的 %s 入参
         * 会被 GCC 的 -Wrestrict 拦下来（它没法证明同结构体里两个数组成员不重叠）。 */
        char name[ALBUM_NAME_MAX];
        const size_t nlen = strlen(ent->d_name);
        if (nlen == 0 || nlen >= sizeof(name)) {
            ESP_UTILS_LOGW("跳过文件名过长的条目（%u 字符）", (unsigned)nlen);
            continue;
        }
        memcpy(name, ent->d_name, nlen + 1);

        Photo &p = _photos[_photo_count];
        memset(&p, 0, sizeof(p));

        memcpy(p.name_buf, name, nlen + 1);
        p.name      = p.name_buf;
        p.from_file = true;

        const int need = snprintf(p.path, sizeof(p.path), "%s/%s", dir, name);
        if (need < 0 || (size_t)need >= sizeof(p.path)) {
            continue;
        }

        struct stat st;
        if (stat(p.path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        /*
         * 读文件头（最多 4 KB）辨认。为什么只读头：这几 MB 的原图整张读进来只为拿个尺寸，
         * 太亏了；而 JPEG 的 SOF、WebP 的 RIFF 头都在文件最前面。
         */
        uint8_t head[4096];
        size_t  got = 0;
        FILE *fp = fopen(p.path, "rb");
        if (fp != nullptr) {
            got = fread(head, 1, sizeof(head), fp);
            fclose(fp);
        }
        const char *sniffed = (got > 0) ? _sniffFormat(head, got) : nullptr;

        /* 记录"这张已经写过占位原因了"，免得下面再覆盖一次 */
        bool noted = false;

        if (sniffed != nullptr && strcmp(sniffed, "JPEG") == 0) {
            if (_jpegInfo(head, got, p.src_w, p.src_h, &p.sof)) {
                p.src_max  = (p.src_w > p.src_h) ? p.src_w : p.src_h;
                p.baseline = (p.sof == 0xC0);
            } else {
                snprintf(p.note_buf, sizeof(p.note_buf), "JPEG\nheader bad");
                p.grid_note = p.note_buf;
                _failed_count++;
                noted = true;
            }
        } else if (sniffed != nullptr && strcmp(sniffed, "WebP") == 0) {
            /*
             * WebP。用一个"只读 RIFF 块头、按块大小 seek"的解析器拿尺寸 ——
             * 4 KB 的 head 里可能被很大的 ICCP/EXIF 块占满，不能假设图像块在前 4 KB 内。
             * 同时拿到有损/无损、alpha、动画标志，用来估算 libwebp 的**内部**内存
             *（它不受我们 MAX_DETAIL_PX 的控制，见 WEBP_INTERNAL_BUDGET）。
             */
            uint8_t wfmt  = 0;
            bool    alpha = false;
            bool    anim  = false;
            if (_webpInfoFile(p.path, p.src_w, p.src_h, wfmt, alpha, anim)) {
                p.src_max        = (p.src_w > p.src_h) ? p.src_w : p.src_h;
                p.baseline       = true;   /* 只对 JPEG 有意义；给个体面的默认值 */
                p.is_webp        = true;
                p.webp_format    = wfmt;
                p.webp_alpha     = alpha;
                p.webp_animation = anim;

                const int64_t internal = (wfmt == 2)       /* VP8L 无损：w*h*4 */
                                         ? (int64_t)p.src_w * p.src_h * 4
                                         : (alpha ? (int64_t)p.src_w * p.src_h : 0);
                if (internal > WEBP_INTERNAL_BUDGET) {
                    /* ASCII，理由见 _sofName() 上面那段（montserrat 没有中文字形）。 */
                    snprintf(p.note_buf, sizeof(p.note_buf), "WebP %s\nneeds %lld MB",
                             (wfmt == 2) ? "lossless" : "alpha",
                             (long long)(internal / (1024 * 1024)));
                    p.grid_note = p.note_buf;
                    _failed_count++;
                    noted = true;
                }
                if (anim) {
                    ESP_UTILS_LOGW("'%s': 动画 WebP —— 相册按首帧显示", name);
                }
            } else {
                snprintf(p.note_buf, sizeof(p.note_buf), "WebP\nheader bad");
                p.grid_note = p.note_buf;
                _failed_count++;
                noted = true;
            }
        }

        if (!noted) {
            if (p.src_max <= 0) {
                /* 解不了就把**真实格式**写出来。
                 * 含糊的 "not a readable JPEG" 帮不上忙 —— 用户实测最典型的情况是
                 * **把 PNG 改成了 .jpg**（安卓截图默认就是 PNG），光看那句话根本想不到。 */
                snprintf(p.note_buf, sizeof(p.note_buf), "%s\nJPEG/WebP only",
                         (sniffed != nullptr) ? sniffed : "unrecognized");
                p.grid_note = p.note_buf;
                _failed_count++;
            } else if (!p.is_webp && !p.baseline) {
                /* 渐进式（SOF2）等。**板上 esp_new_jpeg 只支持基线式**，解不了。
                 * 不静默丢：占一个格子，把**确切的 SOF 标记**写在上面 ——
                 * 光写"渐进式"是猜的，写 SOF 标记才是事实（有了它用户就知道该转成什么）。 */
                snprintf(p.note_buf, sizeof(p.note_buf), "%s\nSOF0 only", _sofName(p.sof));
                p.grid_note = p.note_buf;
                _failed_count++;
            } else if (!p.is_webp && !_isJpegName(name)) {
                /* 扩展名不是 .jpg/.jpeg，但内容确实是基线式 JPEG（有人把 .png 改名成 .jpg，
                 * 反过来也有人只是扩展名写错）。内容说了算，这里只提示一句，不拦。 */
                ESP_UTILS_LOGW("'%s' 的扩展名不像 JPEG，但内容确实是基线式 —— 照常显示", name);
            }
        }
        /* 其余情况：grid_px 还是 nullptr、grid_note 也是 nullptr ⇒ "等着补小图" */

        _photo_count++;
        added++;
    }
    closedir(dp);

    ESP_UTILS_LOGI("目录 %s：%d 张（本目录画不出来 %d 张；本目录因超过上限 %d 张被丢；累计共 %d 张）",
                   dir, added, _failed_count - failed_before, _truncated - truncated_before,
                   _photo_count);
    return (_photo_count > 0);
}

/*
 * 通用解码：把 p 的**编码字节**（JPEG 或 WebP）解到 target_edge（长边），
 * 输出（RGB565 LE）写进调用方给的 buf。
 * target_edge 小于源图长边时走解码器降采样，否则原分辨率输出。
 * 输出宽高按原图等比算、对齐到 16（见 JPEG_ALIGN；WebP 也沿用同一套，保证分配一致）。
 */
bool Album::_decodeTo(const Photo &p, int target_edge, uint8_t *buf, size_t buf_size,
                      int &out_w, int &out_h, int &out_len)
{
    if (p.is_webp) {
        return _decodeWebpBytes(p.data, p.data_len, target_edge, buf, buf_size,
                                out_w, out_h, out_len, p.name);
    }

    int ow = 0;
    int oh = 0;
    if (!_jpegInfo(p.data, p.data_len, ow, oh)) {
        ESP_UTILS_LOGE("'%s': parse jpeg dimension failed", p.name);
        return false;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = ALBUM_JPEG_PIXEL_FMT;

    /* 尺寸一律走 _expectedOutSize —— 调用方按它分配缓冲，两边**不能各算一遍**，
     *    否则迟早漂移（漂移的表现是"图看着是斜的"或者分配不够，都很难查）。
     *    里面用的 _alignJpeg 是 **16** 而不是文档写的 8，见 JPEG_ALIGN 的说明。 */
    const int longest = (ow > oh) ? ow : oh;
    _expectedOutSize(ow, oh, target_edge, out_w, out_h);
    if (target_edge < longest) {
        cfg.scale.width  = (uint16_t)out_w;
        cfg.scale.height = (uint16_t)out_h;
    }
    /* else：不缩放 —— 走解码器的原始分辨率路径（任意宽高都支持，不受 16 约束）。 */

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_UTILS_LOGE("'%s': jpeg_dec_open failed", p.name);
        return false;
    }

    jpeg_dec_io_t io;
    memset(&io, 0, sizeof(io));
    io.inbuf     = const_cast<uint8_t *>(p.data);
    io.inbuf_len = (int)p.data_len;

    jpeg_dec_header_info_t info;
    memset(&info, 0, sizeof(info));
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_UTILS_LOGE("'%s': parse header failed", p.name);
        jpeg_dec_close(dec);
        return false;
    }

    out_len = 0;
    if (jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        ESP_UTILS_LOGE("'%s': get outbuf len failed", p.name);
        jpeg_dec_close(dec);
        return false;
    }
    if ((size_t)out_len > buf_size) {
        ESP_UTILS_LOGE("'%s': need %d B but buf only %u B", p.name, out_len, (unsigned)buf_size);
        jpeg_dec_close(dec);
        return false;
    }

    /*
     * 哨兵守卫（见文件头 JPEG_SAFE_GAP_* 的说明）：把**最后一行**先铺成 0xAA，
     * 解完再回看它还在不在。解码器截断时留下的是**未写**的尾巴，最后一行必然还是哨兵。
     * 扫描量只有一行（几千字节），代价可以忽略；但它是"绝不把半张图显示出去"的底线 ——
     * 万一日后遇到本文没覆盖到的坏尺寸，会走失败分支而不是画花。
     * （真实像素碰巧整行都是 0xAA 的概率可忽略：1280 个像素 ≈ 1/256^2560。）
     */
    const size_t tail = (size_t)out_w * 2u;
    const bool   guard_armed = (tail > 0 && (size_t)out_len >= tail);
    if (guard_armed) {
        memset(buf + (size_t)out_len - tail, 0xAA, tail);
    }

    io.outbuf   = buf;
    io.out_size = out_len;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_UTILS_LOGE("'%s': jpeg_dec_process failed", p.name);
        jpeg_dec_close(dec);
        return false;
    }

    if (guard_armed) {
        const uint8_t *last = buf + (size_t)out_len - tail;
        bool untouched = true;
        for (size_t i = 0; i < tail; i++) {
            if (last[i] != 0xAA) {
                untouched = false;
                break;
            }
        }
        if (untouched) {
            ESP_UTILS_LOGE("'%s': 解码器截断输出（%dx%d 的最后一行没被写）—— 丢弃不用",
                           p.name, out_w, out_h);
            jpeg_dec_close(dec);
            return false;
        }
    }

    jpeg_dec_close(dec);
    return true;
}

/*
 * 通用解码入口：**内嵌图和上传图都能用**。
 * 上传图在这里把整个文件读进 PSRAM 再解 —— 所以这个函数比 _decodeTo 慢一点，
 * 但调用方不用关心图是从哪来的。第一次进大图页、或按倍率重解时走它。
 */
bool Album::_decodeAt(const Photo &p, int target_edge, uint8_t *buf, size_t buf_cap,
                      int &out_w, int &out_h, int &out_len)
{
    if (!p.from_file) {
        return _decodeTo(p, target_edge, buf, buf_cap, out_w, out_h, out_len);
    }

    size_t jpeg_len = 0;
    uint8_t *jpeg = _readFileAlloc(p.path, &jpeg_len);
    if (jpeg == nullptr) {
        ESP_UTILS_LOGE("'%s': 读原图失败", p.name);
        return false;
    }

    /* 借用 _decodeTo 的"从内存解码"接口：临时把文件字节挂到一份拷贝上，
     * 免得为了一个路径去改那个已经验证过的函数。 */
    Photo tmp = p;
    tmp.data     = jpeg;
    tmp.data_len = jpeg_len;

    const bool ok = _decodeTo(tmp, target_edge, buf, buf_cap, out_w, out_h, out_len);
    heap_caps_free(jpeg);
    return ok;
}

/* 网格里该用哪份位图：内嵌图借它自己的大图（只有 2 张，不心疼），上传图用它自己的小图。 */
const lv_image_dsc_t *Album::_gridDsc(const Photo &p)
{
    return p.from_file ? &p.grid_dsc : &p.dsc;
}

int Album::_nameLimit(void) const
{
    const char *n0 = (_photo_count > 0) ? _photos[0].name : nullptr;
    return (n0 != nullptr) ? (int)strlen(n0) : 8;
}

/*
 * 大图页的"基准位图"（100% 时显示的那张）。
 * 内嵌：常驻在 Photo 里；上传：现解进共享槽 _base_buf。**一次只留一张** ——
 * 每张常驻一份的话，1440 的图一张就是 4 MB，几张就把 PSRAM 吃穿了。
 */
bool Album::_ensureBase(int index)
{
    if (index < 0 || index >= _photo_count) {
        return false;
    }
    Photo &p = _photos[index];
    if (!p.from_file) {
        return (p.pixels != nullptr);       /* 内嵌图天生常驻 */
    }
    if (p.grid_note != nullptr) {
        return false;                       /* 占位格：进不了大图页 */
    }
    if (_base_photo == index) {
        return true;                        /* 已经在共享槽里了 */
    }

    const int target = _baseEdgeFor(p.src_w, p.src_h);

    /* 按**真实**输出尺寸算需要多少内存，不再用"长边²"（见 _expectedOutSize 的说明） */
    int ew = 0;
    int eh = 0;
    _expectedOutSize(p.src_w, p.src_h, target, ew, eh);
    const size_t cap = (size_t)ew * (size_t)eh * 2u;

    /*
     * 不够大就**另开一块**，解成功了再换过去、最后才释放旧的。顺序不能反 ——
     *    先释放再分配的话，一旦分配失败，_base_dsc.data 就指着已释放的内存，
     *    而 _full_img 还挂着它 ⇒ **用后释放**（抽到就重启、抽不到就花屏）。
     *    用户实测："两张长图都开启后再切换 —— 第 4 张容易点不开，或快速切换后复位重启"。
     */
    uint8_t *fresh = nullptr;
    if (_base_buf == nullptr || _base_buf_size < cap) {
        fresh = (uint8_t *)heap_caps_aligned_alloc(16, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (fresh == nullptr) {
            ESP_UTILS_LOGE("'%s': base psram alloc %u B failed (PSRAM free %u KB)",
                           p.name, (unsigned)cap,
                           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            return false;       /* 旧缓冲与旧 _base_dsc 都保持原样，不会有悬空指针 */
        }
    }
    uint8_t *work = (fresh != nullptr) ? fresh : _base_buf;
    const size_t work_cap = (fresh != nullptr) ? cap : _base_buf_size;

    const int64_t t0 = esp_timer_get_time();
    int w = 0;
    int h = 0;
    int len = 0;
    if (!_decodeAt(p, target, work, work_cap, w, h, len)) {
        if (fresh != nullptr) {
            heap_caps_free(fresh);
        }
        /* 注意：这里**不改** p.grid_note —— 解不出大图不代表网格里那张小图也坏了，
         * 那张已经解好的小图还能看。只是这一次进不了大图页。 */
        ESP_UTILS_LOGE("'%s': 基准位图解码失败", p.name);
        return false;
    }

    /* 换过去（此时才释放旧的 —— _base_dsc 与 _full_img 都还没有指向新缓冲） */
    if (fresh != nullptr) {
        if (_base_buf != nullptr) {
            heap_caps_free(_base_buf);
        }
        _base_buf = fresh;
        _base_buf_size = cap;
    }

    memset(&_base_dsc, 0, sizeof(_base_dsc));
    _base_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    _base_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    _base_dsc.header.w      = (uint32_t)w;
    _base_dsc.header.h      = (uint32_t)h;
    _base_dsc.header.stride = (uint32_t)(len / h);
    _base_dsc.data_size     = (uint32_t)len;
    _base_dsc.data          = _base_buf;
    _base_w     = w;
    _base_h     = h;
    _base_photo = index;

    ESP_UTILS_LOGI("'%s': 上传图基准 %dx%d（长边目标 %d），读+解共 %lld ms",
                   p.name, w, h, target, (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

const lv_image_dsc_t *Album::_curBaseDsc(void) const
{
    return _photos[_full_index].from_file ? &_base_dsc : &_photos[_full_index].dsc;
}

int Album::_curBaseW(void) const
{
    return _photos[_full_index].from_file ? _base_w : _photos[_full_index].w;
}

int Album::_curBaseH(void) const
{
    return _photos[_full_index].from_file ? _base_h : _photos[_full_index].h;
}

int Album::_curBaseLong(void) const
{
    const int w = _curBaseW();
    const int h = _curBaseH();
    return (w > h) ? w : h;
}

/*
 * "100%" 对应的解码长边：把图**等比缩到刚好装进可视区**（contain），但**不放大**。
 *
 * 为什么不能一律用 DECODE_MAX_EDGE=720：
 *   720 是"长边 720"，对**横图**来说差不多正好铺满屏宽；但**竖图**的约束是**屏高**，
 *   720 白白浪费掉一半高度 —— 而手机拍的照片绝大多数是竖图，所以这个损失是常态。
 *   （用户实测反馈："目前这个 jpg 长图片确实显示得小小的，不合适"。）
 *
 * 不放大（装得下就保持原尺寸）是**刻意**的，也是用户明确要的（"竖图不用铺满，
 * 默认是怎么样就怎么样"）：我们的显示路径是"按解码尺寸 1:1 直绘"，要放大只能让
 * LVGL 逐像素重采样，那是一帧一次的全屏重算。
 * ⇒ 源图比屏幕小的时候，就**如实显示它的原生尺寸**。
 */
int Album::_baseEdgeFor(int src_w, int src_h)
{
    if (src_w <= 0 || src_h <= 0) {
        return DECODE_MAX_EDGE;
    }

    const lv_area_t &va = getVisualArea();
    const int vw = lv_area_get_width(&va);
    const int vh = lv_area_get_height(&va);
    if (vw <= 0 || vh <= 0) {
        return DECODE_MAX_EDGE;
    }

    const int src_long = (src_w > src_h) ? src_w : src_h;

    /* contain：取两个方向收缩比里**小**的那个 ⇒ 整张图都装得下。
     * 用 64 位交叉相乘比较，避免浮点和整数除法误差。 */
    int num = vw;
    int den = src_w;
    if ((int64_t)vh * (int64_t)src_w < (int64_t)vw * (int64_t)src_h) {
        num = vh;
        den = src_h;
    }

    if (num >= den) {
        return src_long;            /* 本来就装得下 ⇒ 原样，别放大 */
    }

    int t = (int)(((int64_t)src_long * num) / den);
    return _alignJpeg(t);           /* 16 的倍数，不是 8 —— 见 JPEG_ALIGN */
}

/*
 * 基准解码：长边按 _baseEdgeFor()（= 100% 尽量铺满可视区，但不放大）。
 * 网格缩略图和大图页的「100%」都用它。
 */
bool Album::_decodeInto(Photo &p)
{
    int ow = 0;
    int oh = 0;
    if (!_jpegInfo(p.data, p.data_len, ow, oh)) {
        ESP_UTILS_LOGE("'%s': parse jpeg dimension failed", p.name);
        return false;
    }
    p.src_w   = ow;                 /* _budgetEdge() 要按源**面积**算，必须留下这两个 */
    p.src_h   = oh;
    p.src_max = (ow > oh) ? ow : oh;

    const int target = _baseEdgeFor(ow, oh);

    /* 多留 16 像素的余量：有些实现会把每行/每列补齐到 16 的倍数 */
    const size_t cap = (size_t)(target + 16) * (size_t)(target + 16) * 2;
    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(16, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        ESP_UTILS_LOGE("'%s': psram alloc %u B failed", p.name, (unsigned)cap);
        return false;
    }

    int out_w = 0;
    int out_h = 0;
    int out_len = 0;
    if (!_decodeTo(p, target, buf, cap, out_w, out_h, out_len)) {
        heap_caps_free(buf);
        return false;
    }

    /* 行跨距：有些实现会把每行补齐，用总长度反推最稳 */
    const int stride = (out_h > 0) ? (out_len / out_h) : 0;

    p.pixels = buf;
    p.w = out_w;
    p.h = out_h;

    memset(&p.dsc, 0, sizeof(p.dsc));
    p.dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    p.dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    p.dsc.header.w      = (uint32_t)out_w;
    p.dsc.header.h      = (uint32_t)out_h;
    p.dsc.header.stride = (uint32_t)stride;
    p.dsc.data_size     = (uint32_t)out_len;
    p.dsc.data          = buf;

    ESP_UTILS_LOGI("'%s': src %dx%d -> base %dx%d (长边目标 %d), %d B, stride %d",
                   p.name, ow, oh, out_w, out_h, target, out_len, stride);
    return true;
}

/*
 * 当前倍率下，**位图**要解码到多长边（注意不是屏幕上要显示多大 —— 那个是 _syncDetail 里的 want）。
 * 下限 = 基准位图长边（比它还小就没必要重解）；上限 = 能**安全**请求的最大尺寸 _safeEdge()。
 */
int Album::_detailTarget(void)
{
    if (_photo_count <= 0 || _full_index >= _photo_count || _zoom_slider == nullptr) {
        return DECODE_MAX_EDGE;
    }
    const Photo &cur = _photos[_full_index];
    const int z = (int)lv_slider_get_value(_zoom_slider);

    /*
     * 一律按**长边**算。_decodeTo 的 target_edge 是长边，而 _syncDetail 摆位置用的是
     *    宽高两个方向 —— 两者只在**正方形**图上等价（内嵌那两张正好都是正方形，
     *    所以早前用 cur.w 那个错一直没露馅）。现在上传图进来，竖图是常态，别改回去。
     */
    const int base_long = _curBaseLong();

    int t = _alignJpeg(base_long * z / 100);   /* 16 的倍数，不是 8 —— 见 JPEG_ALIGN */

    /* ① 解码器安全上限 —— **只针对 esp_new_jpeg**：它会在目标尺寸接近源尺寸时
     *    **静默截断**（见 JPEG_SAFE_GAP_*）。libwebp 没有这个毛病，所以 WebP 直接
     *    放到源长边（再多也没有新细节）。超出上限的倍率由 _syncDetail 用 LVGL 缩放补上。 */
    const int cap = cur.is_webp ? cur.src_max : _safeEdge(cur.src_max);
    if (t > cap) {
        t = cap;
    }
    /* ② 内存预算上限 —— 手机原图在高倍率下会解出一张十几 MB 的位图（见 MAX_DETAIL_PX）。 */
    t = _budgetEdge(cur.src_w, cur.src_h, t);

    if (t < base_long) {
        t = base_long;      /* 没超过基准位图 ⇒ 不必重解（上限比基准还小时也落到这里） */
    }
    return t;
}

/*
 * 防抖后的重解码：倍率稳定下来就把图按目标尺寸重解一次，然后**以 1:1 显示**。
 * 关键：1:1（scale==256）才走 LVGL 的 memcpy 快路径 —— 平移才会丝滑。
 */
void Album::_syncDetail(void)
{
    if (_fullview == nullptr || _full_img == nullptr || _zoom_slider == nullptr || _zoom_view == nullptr) {
        return;
    }
    if (lv_obj_has_flag(_fullview, LV_OBJ_FLAG_HIDDEN)) {
        return;                             /* 不在大图页，不干活 */
    }
    if (_photo_count <= 0 || _full_index >= _photo_count) {
        return;
    }

    const Photo &cur = _photos[_full_index];

    /*
     * 真·防抖：只有滑动条的值**连着两拍没变**（= 手停了）才动手重解码。
     * 这一步之前漏了，后果很严重：拖动时每 250ms 都会触发一次约 0.3 秒的完整重解码，
     *    于是"滑动条比不做这个优化还卡" —— 用户实测报过。别再把这段删掉。
     */
    const int z = (int)lv_slider_get_value(_zoom_slider);
    if (z != _zoom_seen) {
        _zoom_seen = z;
        return;
    }

    const int base_long = _curBaseLong();
    const int want      = base_long * z / 100;      /* 屏幕上要显示的长边 */
    const int target    = _detailTarget();          /* 想解的位图长边（可能被各种上限压回来） */

    /*
     * 摆位置一律用**显示尺寸**，宽高比取自基准位图 —— 于是"拖动中"（_applyZoom 摆的）
     * 和"松手后"（这里摆的）占位**完全一致**，换上明细图时不会跳一下。
     * （drag 那边摆的是 base_w*z/100 × base_h*z/100，和下面这两行是同一个式子。）
     */
    const int disp_w = _curBaseW() * z / 100;
    const int disp_h = _curBaseH() * z / 100;

    /* 已经在显示正是想要的那一张了 —— 这条守卫让"重解/放大"这类重活一拍最多干一次 */
    if (_shown_photo == _full_index && _shown_long == want) {
        return;
    }

    /*
     * ---- 一、挑出手上**最好**的那张位图 ----
     * 能重解就重解（清晰、1:1）；重解不了（目标被上限压回基准 / 刚失败过）就用基准位图顶着。
     */
    const lv_image_dsc_t *src = (target > base_long) ? _ensureDetail(target) : nullptr;
    if (src == nullptr) {
        src = _curBaseDsc();
    }
    if (src == nullptr || src->data == nullptr) {
        return;
    }
    const int src_long   = (int)((src->header.w > src->header.h) ? src->header.w : src->header.h);
    const int src_stride = (int)src->header.stride;

    /*
     * ---- 二、想要比手上这张更大 ⇒ **自己一次性放大到屏幕尺寸** ----
     * 之后按 1:1 显示，平移才走 LVGL 的 memcpy 快路径。
     * 这一条对**两种情况**都成立，缺一不可：
     *   ① 目标尺寸被"解码器安全余量 / 内存预算"压回来（手机原图在高倍率时常见）；
     *   ② **源图本来就装得进屏幕** —— 第 3 张 540×960 就是。那时 `_detailTarget` 会塌回
     *      基准尺寸，**根本没有"更大的位图"可解**，只能放大基准位图本身。
     *
     * 以前漏了②：这类图 100% 以上**永远**在让 LVGL 每帧缩放 ⇒ 又糊又卡、
     *    "卡在缓冲期出不来"，而且**一条日志都不打**（用户 2026-10-13 实测：
     *    "切换到第 3 张图片时，每次放大都是在缓冲期，就基本没出来过"）。
     *    判据就写在数据里：日志里 f277af…（540×960）连一条 detail 都没有。
     */
    if (want > src_long && disp_w > 0 && disp_h > 0) {
        /* **目标只比手上这张大一点点（<3%）时不另开放大缓冲**。
         * 实测 176% 那档：want=2252、明细长边 2240 —— 只差 0.5%，却要另开一块 **4.7 MB**
         * 的放大缓冲。而那块缓冲经常正是"下一次分配分不出来"的那一根稻草。
         * 做法：按**明细图自己的尺寸** 1:1 显示（和缩放条读数差 <3%，肉眼看不出来），
         * 平移照样走 LVGL 的 memcpy 快路径。 */
        if ((int64_t)want * 100 <= (int64_t)src_long * 103) {
            lv_image_set_src(_full_img, src);
            lv_image_set_scale(_full_img, 256);         /* 1:1 */
            _layoutFull((int)src->header.w, (int)src->header.h);
            lv_obj_invalidate(_zoom_view);
            _markDetailReady();
            _shown_photo = _full_index;
            _shown_long  = want;
            ESP_UTILS_LOGI("'%s': @%d%% 目标 %d 与明细长边 %d 差 <3%%，按 1:1 显示"
                           "（省一块放大缓冲）PSRAM free %u KB",
                           cur.name, z, want, src_long,
                           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            return;
        }

        size_t   scaled_len = 0;
        uint8_t *scaled = _ensureScaled(src->data, (int)src->header.w, (int)src->header.h,
                                        src_stride, disp_w, disp_h, &scaled_len);
        if (scaled != nullptr) {
            /*
             * 放大好了 ⇒ **解码缓冲立刻还回去**（那张图已经在放大缓冲里了，它没用了）。
             * 这一步是**内存峰值的关键**：手机原图在 200% 时，解码缓冲 3.8 MB 与放大缓冲
             * 6.6 MB 同时挂着就是 ~12 MB；而放大缓冲"往大里长"时得先释放旧的、再申请更大的，
             * 峰值一高那次申请就会失败 ⇒ 静默退回 LVGL 缩放 ⇒ **平移变卡**。
             * 用户实测报的就是这个："图4 在倍率 ≥175% 后滑动明显感觉不跟手"。
             */
            if (_detail_buf != nullptr) {
                heap_caps_free(_detail_buf);
                _detail_buf = nullptr;
                _detail_buf_size = 0;
            }
            _detail_edge = 0;
            _detail_photo = -1;
            _detail_want = 0;
            memset(&_detail_dsc, 0, sizeof(_detail_dsc));   /* dsc 也一并作废，别留悬空指针 */

            memset(&_shown_dsc, 0, sizeof(_shown_dsc));
            _shown_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            _shown_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            _shown_dsc.header.w      = (uint32_t)disp_w;
            _shown_dsc.header.h      = (uint32_t)disp_h;
            _shown_dsc.header.stride = (uint32_t)disp_w * 2u;
            _shown_dsc.data_size     = (uint32_t)scaled_len;
            _shown_dsc.data          = scaled;

            lv_image_set_src(_full_img, &_shown_dsc);
            lv_image_set_scale(_full_img, 256);     /* ← 1:1，平移走 memcpy 快路径 */
            _layoutFull(disp_w, disp_h);

            /*
             * 强制把**整个视口**标脏重绘一次。
             * 为什么必须有：从"图片比视口大（摆 (0,0) + 滚动）"切到"图片比视口小（重新居中）"
             * 是唯一一次尺寸和位置同时变、而且缩回视口内的场景 —— 只靠增量失效会留下
             * 上一张图的残影，看起来就是"图片里面还有一张图片"。用户实测：
             * "图3 在任意倍率停留然后再滑回 100% 时，图片边框会出现黑影重叠"。
             * 代价只有一次全视口重绘，而且只在换图时发生。
             */
            lv_obj_invalidate(_zoom_view);
            _markDetailReady();             /* 清晰图已换上 ⇒ 开始倒计时，1 秒后才真开闸 */

            _shown_photo = _full_index;
            _shown_long  = want;
            ESP_UTILS_LOGI("'%s': @%d%% 放大显示 %dx%d（1:1，源位图长边 %d）PSRAM free %u KB",
                           cur.name, z, disp_w, disp_h, src_long,
                           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            return;
        }
        /* 放不下（超预算或分配失败）：退回让 LVGL 缩放 —— 略软、平移略慢，但至少不崩。
         * 两条失败原因各自都会打日志（见 _ensureScaled），不会再静默。 */
    }

    /*
     * ---- 三、直接显示手上这张 ----
     * want == src_long 时 scale 正好 256（走 memcpy 快路径）；否则只能是 LVGL 缩放。
     */
    const uint32_t scale = (uint32_t)(256 * want / src_long);
    lv_image_set_src(_full_img, src);
    lv_image_set_scale(_full_img, scale);
    _layoutFull(disp_w, disp_h);
    lv_obj_invalidate(_zoom_view);      /* 同上：换图后强制整视口重绘一次，清掉上一张的残影 */
    _markDetailReady();                 /* 已经是 1:1 了 ⇒ 开始倒计时 */
    _shown_photo = _full_index;
    _shown_long  = want;

    ESP_UTILS_LOGI("'%s': @%d%% 显示 源位图长边=%d 目标=%d 显示 %dx%d scale=%u",
                   cur.name, z, src_long, target, disp_w, disp_h, (unsigned)scale);
}

/*
 * 按 target 重解一张明细位图（结果给 _detail_dsc）。带失败记忆：同一张图的同一个尺寸
 * 失败过一次就不再重撞（防抖定时器每 250ms 跑一次，不记的话就是日志刷爆 + CPU 白烧 +
 * 画面永远卡在预览态，用户实测报过）。
 * 成功返回 dsc；失败或"刚失败过"返回 nullptr（调用方会用基准位图顶着）。
 */
const lv_image_dsc_t *Album::_ensureDetail(int target)
{
    /* 缓存判据用 `_detail_want`（**理想**目标），不是 `_detail_edge`（实际解出来的长边）：
     * 内存不够时会降档解，那时 edge < target，但"这个请求已经满足了"这一点没变。 */
    if (_detail_want == target && _detail_photo == _full_index) {
        return &_detail_dsc;                    /* 已经解好了（可能降过档） */
    }
    if (_detail_fail_photo == _full_index && _detail_fail_target == target) {
        return nullptr;                         /* 刚失败过，别重撞 */
    }

    const Photo &cur = _photos[_full_index];

    /*
     * 按**真实**输出尺寸算内存（见 _expectedOutSize）—— 以前用"长边²"，
     * 对非正方形图浪费好几倍，正是把手机原图撑爆的元凶。
     *
     * **分不出来就降一档解，别直接放弃**。
     * 实测用户场景（视频挂后台 → 进相册 → 拉到 162%）：
     *     detail psram alloc 4,028,928 B failed (PSRAM free 4,008 KB)
     *     largest_free_block = 3,136 KB          ← free 够、**连续块不够**（这是碎片，不是泄漏）
     * 原来这一条失败就直接退回基准位图（1280 长边）让 LVGL 放大到 2064（scale=414），
     * 而 `_shown_long` 已经记成目标值 ⇒ 250 ms 的定时器**不再重试** ⇒ **永远卡在模糊期**。
     * 现在每次分不出来就把目标长边乘 0.8 再试，直到能用（或降到基准尺寸为止）：
     * 最坏情况是清晰度少一两档，但**永远不会停在模糊期**。
     * （每降一档都打一行 WARN —— 别让"降档"变成静默行为。）
     */
    int   out_target = target;      /* 真正解出来的长边（可能被降档） */
    int   ew = 0;
    int   eh = 0;
    size_t need = 0;
    uint8_t *fresh = nullptr;
    const int base_long = (_curBaseW() > _curBaseH()) ? _curBaseW() : _curBaseH();

    for (int attempt = 0; ; attempt++) {
        _expectedOutSize(cur.src_w, cur.src_h, out_target, ew, eh);
        need = (size_t)ew * (size_t)eh * 2u;

        /*
         * 不够大就**另开一块**，解成功了再换过去、最后才释放旧的。顺序不能反 ——
         *    先释放再分配的话，一旦分配失败，_detail_dsc.data 就指着已释放的内存，
         *    而 _full_img 还挂着它 ⇒ **用后释放**（用户实测："快速切换长图有概率重启"）。
         */
        if (_detail_buf != nullptr && _detail_buf_size >= need) {
            break;                  /* 现有缓冲够大，直接复用 */
        }
        fresh = (uint8_t *)heap_caps_aligned_alloc(16, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (fresh != nullptr) {
            break;
        }

        const int next = out_target * 4 / 5;
        if (next <= base_long + 1) {
            ESP_UTILS_LOGE("'%s': detail psram alloc 连降 5 档仍失败（要 %u B，"
                           "PSRAM free %u KB / largest %u KB）",
                           cur.name, (unsigned)need,
                           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
            _detail_fail_photo  = _full_index;  /* 记账！不记就是每 250ms 重撞一次 */
            _detail_fail_target = target;
            return nullptr;
        }
        ESP_UTILS_LOGW("'%s': 长边 %d 的明细图分不出来（要 %u KB，largest %u KB），降档到 %d",
                       cur.name, out_target, (unsigned)(need / 1024),
                       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                       next);
        out_target = next;
    }

    int w = 0;
    int h = 0;
    int len = 0;
    const bool ok = _decodeAt(cur, out_target,
                              (fresh != nullptr) ? fresh : _detail_buf,
                              (fresh != nullptr) ? need : _detail_buf_size,
                              w, h, len);
    if (!ok) {
        if (fresh != nullptr) {
            heap_caps_free(fresh);
        }
        ESP_UTILS_LOGE("'%s': re-decode at %d failed", cur.name, out_target);
        _detail_edge = 0;
        _detail_photo = -1;
        _detail_want = 0;
        _detail_fail_photo  = _full_index;
        _detail_fail_target = target;
        return nullptr;
    }

    if (fresh != nullptr) {
        if (_detail_buf != nullptr) {
            heap_caps_free(_detail_buf);
        }
        _detail_buf = fresh;
        _detail_buf_size = need;
    }

    memset(&_detail_dsc, 0, sizeof(_detail_dsc));
    _detail_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    _detail_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    _detail_dsc.header.w      = (uint32_t)w;
    _detail_dsc.header.h      = (uint32_t)h;
    _detail_dsc.header.stride = (uint32_t)(len / h);
    _detail_dsc.data_size     = (uint32_t)len;
    _detail_dsc.data          = _detail_buf;
    _detail_edge  = out_target;     /* 实际解出来的长边（可能比 target 小） */
    _detail_want  = target;         /* 满足的是哪个**理想**请求 —— 缓存判据用它 */
    _detail_photo = _full_index;

    ESP_UTILS_LOGI("'%s': 重解 %dx%d (%d B)，目标长边 %d（实际 %d），PSRAM free %u KB",
                   cur.name, w, h, len, target, out_target,
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return &_detail_dsc;
}

/*
 * 把 src 一次性放大到 disp_w×disp_h。
 * 为什么值得单独做这一步：LVGL 里只有 scale==256 才走 memcpy 快路径，只要带缩放就是
 * **逐像素重采样**、且代价 ∝ 每帧可见像素数。在这里放大**一次**，
 * 之后的平移就回到快路径 —— 用户实测："放大后（尤其倍率＞150%）滑动明显不跟手"。
 * 超预算或分配失败都返回 nullptr，由调用方退回"让 LVGL 缩放"（慢一点，但画面是对的）。
 */
uint8_t *Album::_ensureScaled(const uint8_t *src, int sw, int sh, int sstride,
                              int disp_w, int disp_h, size_t *out_len)
{
    const int64_t need_px = (int64_t)disp_w * (int64_t)disp_h;
    if (need_px <= 0) {
        return nullptr;
    }
    if (need_px > MAX_DETAIL_PX) {
        ESP_UTILS_LOGW("放大到 %dx%d 超出内存预算（%d px > %d），改由 LVGL 缩放",
                       disp_w, disp_h, (int)need_px, (int)MAX_DETAIL_PX);
        return nullptr;
    }

    const size_t need = (size_t)need_px * 2u;
    if (_scaled_buf == nullptr || _scaled_buf_size < need) {
        uint8_t *fresh = (uint8_t *)heap_caps_aligned_alloc(16, need,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (fresh == nullptr) {
            /* 必须报出来。以前这条分支静默退回 LVGL 缩放，用户看到的就是
             *    "卡在缓冲期出不来（模糊 + 滑动卡顿）"，却查不到任何原因。 */
            ESP_UTILS_LOGE("放大缓冲 %u B 分配失败（PSRAM free %u KB）—— 退回 LVGL 缩放",
                           (unsigned)need,
                           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            return nullptr;
        }
        if (_scaled_buf != nullptr) {
            heap_caps_free(_scaled_buf);
        }
        _scaled_buf = fresh;
        _scaled_buf_size = need;
    }

    const int64_t t0 = esp_timer_get_time();
    if (!_upscaleBilinear(src, sw, sh, sstride, _scaled_buf, disp_w, disp_h)) {
        /* 失败时缓冲里是**未写过**的内容，绝不能交给 lv_image 显示。
         * 返回 nullptr ⇒ 调用方退回"让 LVGL 缩放基准位图"（略慢，但画面是对的）。 */
        return nullptr;
    }
    *out_len = need;
    ESP_UTILS_LOGI("放大 %dx%d -> %dx%d，一次性耗时 %lld ms",
                   sw, sh, disp_w, disp_h, (long long)((esp_timer_get_time() - t0) / 1000));
    return _scaled_buf;
}

/*
 * 按 _full_img 当前的尺寸摆位置：比视口小就居中，比视口大就从左上角摆起（可滚动），
 * 并把视野移到图片中心，免得一放大只看到左上角。
 */
void Album::_layoutFull(int w, int h)
{
    if (_full_img == nullptr || _zoom_view == nullptr || w <= 0 || h <= 0) {
        return;
    }

    /*
     * LVGL 9 里 lv_obj_set_size() / lv_obj_set_pos() 是**延迟生效**的：
     *    它们写的是 style 属性，真正的 coords 要等一次 layout 才算出来
     *    （见 lv_obj_pos.c 的 lv_obj_set_width → lv_style_value_t）。
     * 所以三条规矩，别破：
     *   ① 绝不在 set_size() 之后立刻 lv_obj_get_width() 去读 —— 会读到 0；
     *   ② 居中用的宽高**由调用方算好传进来**，不现查；
     *   ③ set_size 之后先手工 update 一次 layout，后面的滚动才有一致的内部状态。
     * 这三条就是"清理后台后首次点开大图，图片被摆到右下角、切一次图就正常"的根因修法。
     */
    lv_obj_set_size(_full_img, w, h);
    lv_obj_update_layout(_zoom_view);

    const int vw = (_view_w > 0) ? _view_w : lv_obj_get_width(_zoom_view);
    const int vh = (_view_h > 0) ? _view_h : lv_obj_get_height(_zoom_view);

    /* 比视口小 ⇒ 居中；比视口大 ⇒ 从左上角摆起（才有得滚） */
    lv_obj_set_pos(_full_img, (vw > w) ? (vw - w) / 2 : 0,
                               (vh > h) ? (vh - h) / 2 : 0);
    lv_obj_update_layout(_zoom_view);

    /* 放大后把视野摆到图片中心，免得一放大只看到左上角 */
    lv_obj_scroll_to(_zoom_view, (w > vw) ? (w - vw) / 2 : 0,
                                 (h > vh) ? (h - vh) / 2 : 0, LV_ANIM_OFF);
}

void Album::_onZoomSettle(lv_timer_t *t)
{
    (void)t;
    if (_instance == nullptr) {
        return;
    }
    /*
     * 壁纸状态文字到点自动恢复。
     * 复用这个已有的 250 ms 定时器，**不新建 lv_timer** —— 新建的定时器若落在
     * run() 的记账窗口里，关 app 时框架会替我们删一次（同一份资源删两次就是二次释放）。
     */
    if (_instance->_wp_revert_at != 0 &&
            (int32_t)(lv_tick_get() - _instance->_wp_revert_at) >= 0) {
        _instance->_wp_revert_at = 0;
        if (!_instance->_wp_busy.load()) {
            _instance->_wpSetNote("Set Wallpaper", 0xFFFFFF, 0);
        }
    }
    /*
     * 拖动闸什么时候放开，两条路取先到的：
     *   ① 清晰图换上（_pan_ready_at）之后**再等 PAN_GATE_SETTLE_MS** —— 主要走这条；
     *   ② 档位时长到了（2 s / 5 s）—— 兜底，万一清晰图一直换不上（解码失败/内存不够），
     *      也不能把拖动永久锁死。
     * 不能"清晰图一换上就立刻开闸"：手指按着的时候去加回 SCROLLABLE，LVGL 会把
     *    "按下去以来的位移"一次性算进去 ⇒ 猛地一顿。用户实测就是这么发现的。
     */
    if (_instance->_pan_gated) {
        const uint32_t gated_for = lv_tick_elaps(_instance->_pan_started);
        const bool tier_due  = (gated_for >= _instance->_pan_ms);
        const bool ready_due = (_instance->_pan_ready_at != 0) &&
                               (lv_tick_elaps(_instance->_pan_ready_at) >= _instance->_pan_settle_ms);
        if (tier_due) {
            ESP_UTILS_LOGI("拖动闸按档位时长放开（%u ms，清晰图%s）",
                           (unsigned)gated_for,
                           (_instance->_pan_ready_at != 0) ? "已换上" : "一直没换上");
            _instance->_openPanGate();
        } else if (ready_due) {
            _instance->_openPanGate();
        }
    }
    _instance->_syncDetail();
}

/*
 * 拖动闸：调完倍率之后的 ms 毫秒内先不让拖（见头文件里 PAN_GATE_* 的说明）。
 * 关闸用"去掉 LV_OBJ_FLAG_SCROLLABLE"实现 —— 它是最轻的做法，不碰样式、不改布局，
 * 开闸时加回来即可，也不会影响别的对象。
 * ms == 0 表示这一档不需要闸，顺手开闸即可（比如从 180% 拖回 100%）。
 */
void Album::_closePanGate(uint32_t ms, uint32_t settle_ms)
{
    if (ms == 0) {
        _openPanGate();
        return;
    }
    if (_zoom_view == nullptr) {
        return;
    }
    _pan_gated     = true;
    _pan_started   = lv_tick_get();
    _pan_ms        = ms;
    _pan_settle_ms = settle_ms;
    _pan_ready_at  = 0;             /* 新的一轮，清晰图还没换上 */
    lv_obj_remove_flag(_zoom_view, LV_OBJ_FLAG_SCROLLABLE);
}

void Album::_openPanGate(void)
{
    if (!_pan_gated) {
        return;
    }
    _pan_gated    = false;
    _pan_ready_at = 0;
    if (_zoom_view != nullptr) {
        lv_obj_add_flag(_zoom_view, LV_OBJ_FLAG_SCROLLABLE);
        ESP_UTILS_LOGI("拖动闸放开（档位上限 %u ms / 换上后再等 %u ms，实际关了 %u ms）",
                       (unsigned)_pan_ms, (unsigned)_pan_settle_ms,
                       (unsigned)lv_tick_elaps(_pan_started));
    }
}

/* 清晰图换上了：从这里开始再倒计时 _pan_settle_ms 才真开闸（见头文件里的说明）。 */
void Album::_markDetailReady(void)
{
    if (_pan_gated && _pan_ready_at == 0) {
        _pan_ready_at = lv_tick_get();
        ESP_UTILS_LOGI("清晰图已换上，再等 %u ms 开闸（PSRAM free %u KB）",
                       (unsigned)_pan_settle_ms,
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

/* ------------------------------------------------------------------ */
/* 界面：网格页                                                        */
/* ------------------------------------------------------------------ */

void Album::_buildGrid(void)
{
    lv_obj_t *scr = lv_scr_act();
    const lv_area_t &va = getVisualArea();
    const int va_w = lv_area_get_width(&va);
    const int va_h = lv_area_get_height(&va);

    /*
     * 网格 = **可滚动的换行容器**。
     * 原来是"一行平铺"，格子宽 = (可视宽 − 间隔) / 张数 —— 4 张时每格 177 px 还行，
     * 但上传进来之后张数一多，每格会被压到下限然后溢出。所以只要超过 4 张就必须换行。
     */
    _grid = lv_obj_create(scr);
    lv_obj_remove_style_all(_grid);
    lv_obj_set_size(_grid, va_w, va_h);
    lv_obj_set_pos(_grid, va.x1, va.y1);
    lv_obj_set_style_bg_color(_grid, lv_color_hex(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(_grid, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_grid, 16, 0);
    lv_obj_set_style_pad_row(_grid, 16, 0);
    lv_obj_set_flex_flow(_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(_grid, 16, 0);

    /* 标题：整行占满，免得被当成一个格子排进去 */
    lv_obj_t *title = lv_label_create(_grid);
    lv_label_set_text(title, "Album");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8EAED), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_width(title, LV_PCT(100));

    /*
     * 格子尺寸：图片框**正好等于网格小图的长边 GRID_EDGE** ——
     * 这样 LV_IMAGE_ALIGN_CONTAIN 不会真的缩放，绘制走 memcpy 快路径，
     * 滚动才不会卡（内嵌图借用的是它自己的大图，会缩，但只有 2 张，静态绘制无所谓）。
     */
    const int box   = GRID_EDGE;
    const int cell_w = box + 12;
    const int cell_h = box + 12 + 34;   /* 底部留一条给文件名 */

    for (int i = 0; i < _photo_count; i++) {
        const Photo &p = _photos[i];

        lv_obj_t *cell = lv_obj_create(_grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1F26), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        /* 占位文字（先建，图片盖在它上面；两者按需显示其中一个） */
        _cell_note[i] = lv_label_create(cell);
        lv_obj_set_width(_cell_note[i], box - 12);
        lv_label_set_long_mode(_cell_note[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(_cell_note[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(_cell_note[i], lv_color_hex(0xFF8A80), 0);
        lv_obj_set_style_text_font(_cell_note[i], &lv_font_montserrat_16, 0);
        lv_obj_align(_cell_note[i], LV_ALIGN_TOP_MID, 0, 8 + box / 2 - 20);

        _cell_img[i] = lv_image_create(cell);
        lv_obj_set_size(_cell_img[i], box, box);
        lv_obj_align(_cell_img[i], LV_ALIGN_TOP_MID, 0, 6);
        lv_image_set_inner_align(_cell_img[i], LV_IMAGE_ALIGN_CONTAIN);
        /* 还没小图、又没写原因 ⇒ "等着补"，先藏起来，别显示成一张空图 */
        lv_obj_add_flag(_cell_img[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *cap = lv_label_create(cell);
        char shown_name[ALBUM_NAME_MAX + 4];
        _shortName(p.name, _nameLimit(), shown_name, sizeof(shown_name));
        lv_label_set_text(cap, shown_name);
        lv_obj_set_style_text_color(cap, lv_color_hex(0xC7CBD1), 0);
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(cap, cell_w - 12);
        lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
        lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -8);

        lv_obj_add_event_cb(cell, _onThumbClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* 把已有的小图/占位原因挂上去；还没补的交给 _fillNextThumb() 一拍一张地补 */
    _syncThumbCells();
    if (_thumb_timer != nullptr) {
        lv_timer_resume(_thumb_timer);
    }

    /*
     * 底部：把实测内存占用直接显示在屏幕上。
     * 为什么放屏幕上而不是打日志 —— "看不到日志"这条理由早就不成立了（控制台在 2 Mbaud），
     * 但屏幕上的这一行**验收时一眼可见**，比翻串口快。
     */
    lv_obj_t *foot = lv_label_create(_grid);
    lv_obj_set_width(foot, LV_PCT(100));
    if (_failed_count > 0 || _truncated > 0) {
        /* 有图没解出来、或有图被上限丢掉，就红字报出来，别让失败/截断静默发生 */
        lv_label_set_text_fmt(
            foot, "FAILED %d   DROPPED(over limit) %d   PSRAM free %.1f MB",
            _failed_count, _truncated,
            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0 * 1024.0));
        lv_obj_set_style_text_color(foot, lv_color_hex(0xFF6B6B), 0);
    } else {
        lv_label_set_text_fmt(
            foot, "%d photo(s)   PSRAM free %.1f MB   internal free %u KB",
            _photo_count,
            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0 * 1024.0),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        lv_obj_set_style_text_color(foot, lv_color_hex(0x6B7280), 0);
    }
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_16, 0);
}

/*
 * 把小图/占位原因挂到对应格子上。
 * 传 nullptr 给 lv_image_set_src 之前必须换掉 —— LVGL 会当非法源处理。
 */
void Album::_syncThumbCells(void)
{
    for (int i = 0; i < _photo_count && i < MAX_PHOTOS; i++) {
        if (_cell_img[i] == nullptr || _cell_note[i] == nullptr) {
            continue;
        }
        const Photo &p = _photos[i];

        if (p.grid_note != nullptr) {
            /* 画不出来：把**原因**写在格子上（不是静默丢） */
            lv_label_set_text(_cell_note[i], p.grid_note);
            lv_obj_remove_flag(_cell_note[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_cell_img[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const lv_image_dsc_t *dsc = _gridDsc(p);
        if (dsc == nullptr || dsc->data == nullptr) {
            /* 还没轮到解这张：藏起图片、也藏起文字，格子先空着 */
            lv_obj_add_flag(_cell_note[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_cell_img[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_image_set_src(_cell_img[i], dsc);
        lv_obj_remove_flag(_cell_img[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_cell_note[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(_cell_img[i]);
    }
}

/*
 * 懒解码：一拍补一张网格小图，补完自己停。
 *
 * 为什么要懒：一张几 MB 的图，读盘 + 解码要几百毫秒。开机后第一次进相册时
 * 如果一口气把 N 张全解完，界面会僵住好几秒。**先让占位格顶上，再一拍一张地补** ——
 * 用户看到的是"格子陆续亮起来"，而不是"点了相册没反应"。
 * 小图**只存内存、不落盘**：早期那套上传时预生成 .thumbs 的路子已经判过回退，别捡回来。
 */
void Album::_fillNextThumb(void)
{
    for (int i = 0; i < _photo_count; i++) {
        Photo &p = _photos[i];
        if (!p.from_file || p.grid_note != nullptr || p.grid_px != nullptr) {
            continue;   /* 不是上传图 / 已判失败 / 已经解好了 */
        }

        const int64_t t0 = esp_timer_get_time();

        const size_t cap = (size_t)(GRID_EDGE + 16) * (size_t)(GRID_EDGE + 16) * 2u;
        uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(16, cap,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == nullptr) {
            p.grid_note = "out of memory";
            _failed_count++;
            return;
        }

        int w = 0;
        int h = 0;
        int len = 0;
        if (!_decodeAt(p, GRID_EDGE, buf, cap, w, h, len)) {
            heap_caps_free(buf);
            p.grid_note = "decode failed";
            _failed_count++;
            ESP_UTILS_LOGE("'%s': 网格小图解码失败", p.name);
            return;
        }

        p.grid_px  = buf;
        p.grid_w   = w;
        p.grid_h   = h;
        memset(&p.grid_dsc, 0, sizeof(p.grid_dsc));
        p.grid_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        p.grid_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        p.grid_dsc.header.w      = (uint32_t)w;
        p.grid_dsc.header.h      = (uint32_t)h;
        p.grid_dsc.header.stride = (uint32_t)(len / h);
        p.grid_dsc.data_size     = (uint32_t)len;
        p.grid_dsc.data          = buf;

        ESP_UTILS_LOGI("'%s': 网格小图 %dx%d，读+解共 %lld ms",
                       p.name, w, h, (long long)((esp_timer_get_time() - t0) / 1000));
        return;     /* 一拍一张 */
    }
}

void Album::_onThumbTick(lv_timer_t *t)
{
    (void)t;
    if (_instance == nullptr || _instance->_grid == nullptr) {
        return;     /* 不在前台的兜底：_grid 为 null 说明屏幕已经没了，别碰任何 lv 对象 */
    }
    /* 还没补齐就补一张；补齐了就把自己停掉，别白跑 */
    for (int i = 0; i < _instance->_photo_count; i++) {
        const Photo &p = _instance->_photos[i];
        if (p.from_file && p.grid_note == nullptr && p.grid_px == nullptr) {
            _instance->_fillNextThumb();
            _instance->_syncThumbCells();
            return;
        }
    }
    lv_timer_pause(t);
}

/* ------------------------------------------------------------------ */
/* 界面：大图页                                                        */
/* ------------------------------------------------------------------ */

void Album::_openFull(int index)
{
    if (_photo_count <= 0) {
        return;
    }
    const int idx = ((index % _photo_count) + _photo_count) % _photo_count;

    /*
     * 上传图在这里"把基准位图解出来" —— **点开才算**，这就是懒的那一半。
     * 必须放在下面读基准尺寸之前：还没解出来时 _curBaseW/H() 是 0，拿它算什么都错。
     * 顺便把"占位格"挡在门外：解不了的图不该能进大图页。
     *
     * 先确保成功、**再**改 _full_index：失败时 _base_dsc 里还是上一张的像素，
     *    这时候把 _full_index 换过去，_curBaseDsc() 就会拿上一张的图当这一张显示。
     */
    if (!_ensureBase(idx)) {
        ESP_UTILS_LOGW("第 %d 张打不开（位图没就绪），大图页保持原样", idx);
        return;
    }
    _full_index = idx;

    const lv_area_t &va = getVisualArea();
    const int va_w = lv_area_get_width(&va);
    const int va_h = lv_area_get_height(&va);
    const int bar_h = 90;

    if (_fullview == nullptr) {
        _fullview = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(_fullview);
        lv_obj_set_size(_fullview, va_w, va_h);
        lv_obj_set_pos(_fullview, va.x1, va.y1);
        lv_obj_set_style_bg_color(_fullview, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(_fullview, LV_OPA_COVER, 0);
        lv_obj_remove_flag(_fullview, LV_OBJ_FLAG_SCROLLABLE);

        /*
         * 视口：一个**可滚动**的容器，图片是它的子对象。
         * 这样放大之后**直接拖拽就是平移**（LVGL 原生滚动），不需要任何手势识别，
         * 也就不必去开 LV_USE_GESTURE_RECOGNITION / LV_USE_FLOAT（那会影响所有 app）。
         * remove_style_all 是为了去掉默认主题的布局，好让下面的显式定位可控。
         */
        _zoom_view = lv_obj_create(_fullview);
        lv_obj_remove_style_all(_zoom_view);
        /*
         * 视口**铺满整屏**（原来是 va_w−ZOOM_BAR_W × va_h−bar_h，把右侧和底部让出去了）。
         * 现在右侧倍率条 / 底部翻页栏 / Back 按钮一律**浮在图片上方** —— 它们都是在
         * _zoom_view **之后**创建的，LVGL 里后建的在上层、命中也先打它们，
         * 所以"图片可延伸到控件底下 + 控件浮空"这两件事都不用额外做。
         */
        lv_obj_set_size(_zoom_view, va_w, va_h);
        lv_obj_set_pos(_zoom_view, 0, 0);
        /*
         * 记下视口尺寸。**不要**在 _layoutFull() 里现查 —— 首次打开时这个对象刚创建、
         * LVGL 还没排版，lv_obj_get_width/height() 会返回 0，居中就退化成贴顶
         * （用户报的"第一次打开不居中、切一次图再回来就正常"就是这个）。
         */
        _view_w = va_w;
        _view_h = va_h;
        lv_obj_set_style_bg_color(_zoom_view, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(_zoom_view, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(_zoom_view, 0, 0);
        lv_obj_add_flag(_zoom_view, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(_zoom_view, LV_DIR_ALL);
        lv_obj_set_scrollbar_mode(_zoom_view, LV_SCROLLBAR_MODE_OFF);

        _full_img = lv_image_create(_zoom_view);
        /*
         * 关掉抗锯齿（本工程 LV_COLOR_DEPTH=16，默认是**开**的）。
         * 拖动时的预览走重采样路径，关掉能省 1/2~1/3 算力；
         * 停手后换上的是 1:1 明细图，走 memcpy，抗锯齿与否本来就不影响。
         */
        lv_image_set_antialias(_full_img, false);

        /* 侧边竖向倍率滑动条 */
        _zoom_slider = lv_slider_create(_fullview);
        lv_slider_set_orientation(_zoom_slider, LV_SLIDER_ORIENTATION_VERTICAL);
        lv_slider_set_range(_zoom_slider, ZOOM_MIN, ZOOM_MAX);
        lv_slider_set_value(_zoom_slider, ZOOM_MIN, LV_ANIM_OFF);
        lv_obj_set_size(_zoom_slider, 14, va_h - bar_h - 260);
        lv_obj_align(_zoom_slider, LV_ALIGN_TOP_RIGHT, -(ZOOM_BAR_W - 14) / 2, 100);
        lv_obj_add_event_cb(_zoom_slider, _onZoomChanged, LV_EVENT_VALUE_CHANGED, nullptr);

        /* 当前倍率读数（放在滑动条下方，贴着底栏上沿） */
        _zoom_label = lv_label_create(_fullview);
        lv_obj_set_style_text_color(_zoom_label, lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_style_text_font(_zoom_label, &lv_font_montserrat_16, 0);
        lv_obj_align(_zoom_label, LV_ALIGN_BOTTOM_RIGHT, -(ZOOM_BAR_W - 40) / 2, -(bar_h + 16));

        /* 返回按钮（浮在图片左上角） */
        lv_obj_t *back_btn = lv_obj_create(_fullview);
        lv_obj_remove_style_all(back_btn);
        lv_obj_set_size(back_btn, 96, 52);
        lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 16, 16);
        lv_obj_set_style_radius(back_btn, 26, 0);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x1A1F26), 0);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_70, 0);
        lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *back_txt = lv_label_create(back_btn);
        lv_label_set_text(back_txt, "Back");
        lv_obj_set_style_text_color(back_txt, lv_color_hex(0xE8EAED), 0);
        lv_obj_set_style_text_font(back_txt, &lv_font_montserrat_16, 0);
        lv_obj_center(back_txt);
        lv_obj_add_event_cb(back_btn, _onBackToGridClicked, LV_EVENT_CLICKED, nullptr);

        /* 底部：上一张 / 计数 / 下一张 */
        lv_obj_t *bar = lv_obj_create(_fullview);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, va_w, bar_h);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x0B0E12), 0);
        /* 半透明而不是不透明：它就是"浮在图片上方"的一条 ——
         * 全不透明会把图片最下面一整条挡死，看起来像被裁了。 */
        lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(bar, 48, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *prev = lv_obj_create(bar);
        lv_obj_remove_style_all(prev);
        lv_obj_set_size(prev, 88, 56);
        lv_obj_set_style_radius(prev, 12, 0);
        lv_obj_set_style_bg_color(prev, lv_color_hex(0x273040), 0);
        lv_obj_set_style_bg_opa(prev, LV_OPA_COVER, 0);
        lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *prev_txt = lv_label_create(prev);
        lv_label_set_text(prev_txt, "<");
        lv_obj_set_style_text_color(prev_txt, lv_color_hex(0xE8EAED), 0);
        lv_obj_set_style_text_font(prev_txt, &lv_font_montserrat_20, 0);
        lv_obj_center(prev_txt);
        lv_obj_add_event_cb(prev, _onPrevClicked, LV_EVENT_CLICKED, nullptr);

        _full_caption = lv_label_create(bar);
        lv_obj_set_style_text_color(_full_caption, lv_color_hex(0xE8EAED), 0);
        lv_obj_set_style_text_font(_full_caption, &lv_font_montserrat_20, 0);

        lv_obj_t *next = lv_obj_create(bar);
        lv_obj_remove_style_all(next);
        lv_obj_set_size(next, 88, 56);
        lv_obj_set_style_radius(next, 12, 0);
        lv_obj_set_style_bg_color(next, lv_color_hex(0x273040), 0);
        lv_obj_set_style_bg_opa(next, LV_OPA_COVER, 0);
        lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *next_txt = lv_label_create(next);
        lv_label_set_text(next_txt, ">");
        lv_obj_set_style_text_color(next_txt, lv_color_hex(0xE8EAED), 0);
        lv_obj_set_style_text_font(next_txt, &lv_font_montserrat_20, 0);
        lv_obj_center(next_txt);
        lv_obj_add_event_cb(next, _onNextClicked, LV_EVENT_CLICKED, nullptr);

        /*
         * 设为壁纸。文字**必须全 ASCII** —— montserrat 里没有中文字形，
         * 写成"设为壁纸"在屏幕上是乱码（这里栽过一次）。
         * 它和 prev/next 在同一个 flex 行里，所以天然就在"切换图片那块区域"。
         */
        _wp_btn = lv_obj_create(bar);
        lv_obj_remove_style_all(_wp_btn);
        lv_obj_set_size(_wp_btn, 200, 56);
        lv_obj_set_style_radius(_wp_btn, 12, 0);
        lv_obj_set_style_bg_color(_wp_btn, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_bg_opa(_wp_btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(_wp_btn, LV_OBJ_FLAG_CLICKABLE);
        _wp_label = lv_label_create(_wp_btn);
        lv_label_set_text(_wp_label, "Set Wallpaper");
        lv_obj_set_style_text_color(_wp_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(_wp_label, &lv_font_montserrat_16, 0);
        lv_obj_center(_wp_label);
        lv_obj_add_event_cb(_wp_btn, _onSetWallpaperClicked, LV_EVENT_CLICKED, nullptr);
    }

    /*
     * 缩放与平移的设计（2026-10-12 起）：
     *   - 倍率由侧边滑动条控制，走 lv_image_set_scale()，**单点触摸就够**；
     *   - 平移靠 _zoom_view 的原生滚动，拖拽即可，**不需要手势识别**；
     *   ⇒ 因此**不用**打开 LV_USE_GESTURE_RECOGNITION / LV_USE_FLOAT（那会改动整个 LVGL，
     *     影响所有 app）。全部 UI 对象都只属于本 app，别的 app 在前台时它们不存在。
     *
     * 代价：缩放是**纯软件逐像素重采样**（本工程 PPA 未开）。LVGL 9.5 的变换不会为整图
     *   分配缓冲，每帧只要一条 ~6.4 KB 的横带 —— 所以代价是 **CPU 时间**，不是内存。
     *   （早前把"卡死"归因于 CONFIG_LV_CACHE_DEF_SIZE=0 **是错的**：那管的是 JPEG 解码缓存，
     *     而我们的图已是 PSRAM 里的裸 RGB565，绘制时不需要再解码。）
     *   ⇒ 帧率必须实测；若拖滑动条发卡，先降 ZOOM_MAX 或减小 DECODE_MAX_EDGE。
     */
    const Photo &cur = _photos[_full_index];

    /*
     * 滑动条范围**永远**是 ZOOM_MIN..ZOOM_MAX，**不按源分辨率收窄**。
     *
     * 这里踩过坑（2026-10-13 用户实测："长图片的滑动条无法拉动了，能点到但拖不动"）：
     *   上一版把上限算成 `src_max * 100 / 基准长边`。当图**本来就装得下**时
     *   （基准长边 == 源长边，竖图尤其常见），这个式子正好得 100 ⇒ 范围退化成 100~100
     *   ⇒ 滑动条能看见、能点、**就是拖不动**，而且和"坏了"分不清。
     * 现在的取舍：超过源分辨率的那些倍率就是把像素吹大（没有新细节），但**控件必须能用** ——
     * 用户明确要求"保持竖图 or 横图均能用滑动条来滑动缩放"。
     * （范围在创建时已经设过一次，这里不再重复 set_range。）
     */

    /* 换图：退回基准位图、倍率复位、明细图作废（失败记忆也一起清） */
    _detail_edge = 0;
    _detail_photo = -1;
    _detail_want = 0;
    _detail_fail_photo = -1;
    _detail_fail_target = -1;
    _shown_photo = -1;
    _shown_long  = 0;
    lv_image_set_src(_full_img, _curBaseDsc());
    lv_slider_set_value(_zoom_slider, ZOOM_MIN, LV_ANIM_OFF);
    _applyZoom();

    /* 底部那条也要截 —— 手机截图那种 50 字符的名字会把 Back/翻页挤爆 */
    char cap_name[ALBUM_NAME_MAX + 4];
    _shortName(cur.name, _nameLimit(), cap_name, sizeof(cap_name));
    lv_label_set_text_fmt(_full_caption, "%d / %d  %s",
                          _full_index + 1, _photo_count, cap_name);

    lv_obj_add_flag(_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(_fullview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_fullview);
}

void Album::_closeFull(void)
{
    /*
     * 退出大图页 = **把这些大缓冲全放掉**。
     * 一张手机原图在高倍率下要十几 MB（基准 + 明细 + 放大），一直留着的话，
     * 逛一圈相册再进来就可能在别处分配失败 —— 用户实测："两张长图都开启后再切换，
     * 第 4 张容易点不开，或快速切换后复位重启"。
     * 用户的意思也是这个："每个图片每次单独刷新，切换图片后释放上一张所占的缓存"。
     * 代价是再进大图页要重解一次基准位图（上传图本来就是"点开才解"，没多花）。
     */
    _releaseViewer();

    if (_fullview != nullptr) {
        lv_obj_add_flag(_fullview, LV_OBJ_FLAG_HIDDEN);
    }
    if (_grid != nullptr) {
        lv_obj_remove_flag(_grid, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_UTILS_LOGI("退出大图页，已释放大缓冲（PSRAM free %u KB）",
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void Album::_releaseBigBuffers(void)
{
    if (_detail_buf != nullptr) {
        heap_caps_free(_detail_buf);
        _detail_buf = nullptr;
        _detail_buf_size = 0;
    }
    if (_scaled_buf != nullptr) {
        heap_caps_free(_scaled_buf);
        _scaled_buf = nullptr;
        _scaled_buf_size = 0;
    }
    if (_base_buf != nullptr) {
        heap_caps_free(_base_buf);
        _base_buf = nullptr;
        _base_buf_size = 0;
    }
    _base_w = 0;
    _base_h = 0;
    _base_photo = -1;
}

/*
 * 按当前滑动条的倍率重排大图。
 * 显式设图片对象的尺寸 + 位置（不用 lv_obj_align 居中超大子对象），
 * 因为 _zoom_view 已经被 remove_style_all 去掉了默认布局，显式定位最可预测。
 */
void Album::_applyZoom(void)
{
    if (_full_img == nullptr || _zoom_view == nullptr || _zoom_slider == nullptr) {
        return;
    }
    if (_photo_count <= 0 || _full_index >= _photo_count) {
        return;
    }

    const int z = (int)lv_slider_get_value(_zoom_slider);

    if (_zoom_label != nullptr) {
        lv_label_set_text_fmt(_zoom_label, "%d%%", z);
    }

    /*
     * 拖动中的**即时预览**：一律退回基准位图 + LVGL 缩放。
     * 这条路径必然走逐像素重采样（慢），但它只在你拖动的过程中出现；
     * 停手 250ms 后 _syncDetail() 会按倍率重解码一张 1:1 的图换上，
     * 之后的平移就走 memcpy 快路径了 —— 那才是"跟手"的关键。
     */
    if (_detail_edge != 0) {
        _detail_edge = 0;
        _detail_photo = -1;
        _detail_want = 0;
        _detail_want = 0;
        lv_image_set_src(_full_img, _curBaseDsc());
    }
    lv_image_set_scale(_full_img, (uint32_t)(256 * z / 100));
    _layoutFull(_curBaseW() * z / 100, _curBaseH() * z / 100);

    /* 手上挂的已经不是"停手后那一张"了 ⇒ 让 _syncDetail 的守卫失效，
     * 否则停手后它会以为"已经是想要的那张"，明细图就永远换不上。 */
    _shown_photo = -1;
    _shown_long  = 0;

    /* 关闸：现在显示的还是"临时缩放出来的预览图"，拖它必然卡。等清晰图换上再放开。
     * 分三档（用户 2026-10-13 指定）：
     *   ≤130%      不设闸（这一段重解很快，感觉不到）
     *   130%~150%  档位 2 s，清晰图换上后再等 1 s
     *   >150%      档位 5 s，清晰图换上后再等 2 s（这一段重解最久、最容易抖）
     * 目标是**用时间把出错的概率压下去**，不是根治 —— 见头文件里的说明。 */
    uint32_t gate_ms   = 0;
    uint32_t settle_ms = 0;
    if (z > PAN_GATE_HIGH_Z) {
        gate_ms   = PAN_GATE_HIGH_MS;
        settle_ms = PAN_GATE_SETTLE_HIGH_MS;
    } else if (z > PAN_GATE_MID_Z) {
        gate_ms   = PAN_GATE_MID_MS;
        settle_ms = PAN_GATE_SETTLE_MS;
    }
    _closePanGate(gate_ms, settle_ms);
}

/* ------------------------------------------------------------------ */
/* 壁纸                                                         */
/* ------------------------------------------------------------------ */

namespace {

/*
 * 壁纸落盘位置。
 *
 * **优先 SD 卡，/data 只是退路**。实测往 /data（flash + 磨损均衡）写 2 MB 要
 *    21~26 秒，而这段时间里 **CPU 从 flash 取指会被写操作反复打断**：
 *    同一时刻打开一张 JPEG 的基准解码从正常 **150 ms** 变成 **13,851 / 21,527 ms**
 *    （2026-10-19 实测）。用户感受就是"点图片卡 10~20 秒"。
 *    SD 卡走的是另一条总线，写 2 MB 通常 1~4 秒，且不打断 XIP。
 *
 * 文件名以 "." 开头 ⇒ 相册扫描时会跳过（它本来就不扫隐藏文件）。
 * 读的时候**两个位置都试**：SD 优先；卡拔了还能读到 /data 里上一次的退路副本。
 *
 * **按逻辑分辨率分文件存** —— 横屏 1280x800 与竖屏 800x1280 各一份，
 * 互不覆盖，切回某个方向时自动恢复那个方向上次设的壁纸。
 * 旧版单文件（0.5 版及以前）作为**只读退路**保留在候选列表里：升级后第一次开机
 * 还能看到原来的壁纸，设一次新的就自然迁到带尺寸的新文件名上。
 */
constexpr const char *WP_LEGACY_PATH_SD   = "/sdcard/.wallpaper.rgb565";
constexpr const char *WP_LEGACY_PATH_DATA = "/data/wallpaper.rgb565";

/* 当前方向对应的四个路径（正式文件 + 写盘临时文件，SD 与 /data 各一套）。 */
struct WpPaths {
    char sd[64];
    char sd_tmp[64];
    char data[64];
    char data_tmp[64];
};

WpPaths _wpPaths(int w, int h)
{
    WpPaths p {};
    snprintf(p.sd, sizeof(p.sd), "/sdcard/.wallpaper_%dx%d.rgb565", w, h);
    snprintf(p.sd_tmp, sizeof(p.sd_tmp), "/sdcard/.wallpaper_%dx%d.tmp", w, h);
    snprintf(p.data, sizeof(p.data), "/data/wallpaper_%dx%d.rgb565", w, h);
    snprintf(p.data_tmp, sizeof(p.data_tmp), "/data/wallpaper_%dx%d.tmp", w, h);
    return p;
}
constexpr uint32_t    WP_MAGIC    = 0x50573450u;   /* "P4WP"（按小端 u32 读回来） */
constexpr uint16_t    WP_VERSION  = 1;
constexpr uint16_t    WP_FMT_RGB565_LE = 0;

/*
 * 落盘格式：20 字节头 + 紧密排列的 RGB565。
 * 像素是**按 LVGL 逻辑方向**存的（横屏 1280×800 / 竖屏 800×1280 各一个文件），
 * 不是面板物理方向 —— 旋转在 flush 路径上由 PPA 做，与壁纸数据无关。
 * 开机恢复时仍然用当前逻辑分辨率做双重校验：文件名对不上就不读，头里的 w/h
 * 或 data_size 对不上就跳过 —— 宁可没壁纸，也不拿错尺寸去画。
 */
struct WallpaperHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t format;
    uint16_t width;
    uint16_t height;
    uint32_t data_size;
    uint32_t crc32;
} __attribute__((packed));
static_assert(sizeof(WallpaperHeader) == 20, "WallpaperHeader 必须是 20 字节");

/*
 * 常驻壁纸缓冲。**永远不 free**：
 *   - 桌面主屏的 bg_image_src 直接指着它 ⇒ 释放就是 use-after-free；
 *   - 它不归框架的 enable_recycle_resource 管（不是 LVGL 建的对象）。
 * 尺寸 = 逻辑分辨率 × 2（当前 1280×800 = 2,048,000 B）。
 */
struct WallpaperStore {
    uint8_t       *buf = nullptr;
    size_t         cap = 0;
    int            w = 0;
    int            h = 0;
    lv_image_dsc_t dsc {};
};
WallpaperStore s_wp;

SemaphoreHandle_t s_wp_sem = nullptr;        /* 唤醒 worker */
SemaphoreHandle_t s_wp_job_mutex = nullptr;  /* 保护 Album::_wp_job */
TaskHandle_t      s_wp_task = nullptr;       /* 懒创建一次 */
/* 落盘**文件**的互斥量：解码 worker（同步兜底）和保存任务可能同时想写
 * /data/wallpaper.tmp，必须串行 —— 否则两个 25 秒的写会互相覆盖。 */
SemaphoreHandle_t s_wp_save_io = nullptr;

/* 在源图里取一个**居中的、宽高比为 w:h 的最大矩形**（cover 裁剪）。 */
void _coverRect(int sw, int sh, int w, int h, int &cw, int &ch, int &cx, int &cy)
{
    if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) {
        cw = ch = cx = cy = 0;
        return;
    }
    if ((int64_t)sw * h >= (int64_t)sh * w) {
        ch = sh;                                    /* 源更宽：高度用满 */
        cw = (int)((int64_t)sh * w / h);
    } else {
        cw = sw;                                    /* 源更高：宽度用满 */
        ch = (int)((int64_t)sw * h / w);
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (cw > sw) cw = sw;
    if (ch > sh) ch = sh;
    cx = (sw - cw) / 2;
    cy = (sh - ch) / 2;
}

/* 填好 dsc 并挂到主屏上。**调用方必须持有 LVGL 锁。** */
bool _wpApply(lv_obj_t *main_screen)
{
    if (main_screen == nullptr || s_wp.buf == nullptr || s_wp.w <= 0 || s_wp.h <= 0) {
        return false;
    }
    memset(&s_wp.dsc, 0, sizeof(s_wp.dsc));
    s_wp.dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_wp.dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_wp.dsc.header.w      = (uint32_t)s_wp.w;
    s_wp.dsc.header.h      = (uint32_t)s_wp.h;
    s_wp.dsc.header.stride = (uint32_t)s_wp.w * 2u;
    s_wp.dsc.data_size     = (uint32_t)((size_t)s_wp.w * (size_t)s_wp.h * 2u);
    s_wp.dsc.data          = s_wp.buf;

    lv_obj_set_style_bg_image_src(main_screen, &s_wp.dsc, 0);
    /* 这三项显式写死：不依赖主题默认值，也挡住将来样式表改动 */
    lv_obj_set_style_bg_image_opa(main_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(main_screen, false, 0);
    lv_obj_set_style_bg_image_recolor_opa(main_screen, LV_OPA_TRANSP, 0);
    lv_obj_invalidate(main_screen);
    return true;
}

/* SD 卡是否挂着（用目录存在来判断；挂载点用 BSP 的宏，别写死）。 */
bool _wpSdMounted(void)
{
    struct stat st;
    return (stat(BSP_SD_MOUNT_POINT, &st) == 0) && S_ISDIR(st.st_mode);
}

/* 找到当前方向现存的那份壁纸：新文件名 SD 优先 → /data，最后才试旧版单文件。
 * 找到就把路径拷进 out（这些路径是运行时按尺寸拼出来的，不能返回栈上指针）。 */
bool _wpPickExisting(int w, int h, char *out, size_t out_n)
{
    const WpPaths p = _wpPaths(w, h);
    const char *candidates[] = {
        p.sd,
        p.data,
        WP_LEGACY_PATH_SD,
        WP_LEGACY_PATH_DATA,
    };

    struct stat st;
    for (const char *candidate : candidates) {
        if ((stat(candidate, &st) == 0) && S_ISREG(st.st_mode)) {
            snprintf(out, out_n, "%s", candidate);
            return true;
        }
    }
    return false;
}

/* 只读头：开机恢复时先校验尺寸，避免为一版不匹配的壁纸白分配 2 MB。 */
bool _wpReadHeader(const char *path, WallpaperHeader &hdr)
{
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    const bool ok = (fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr));
    fclose(fp);
    return ok;
}

/*
 * 把已落盘的壁纸读回 dst（头、尺寸、CRC 全校验）。
 * 用途只有一处：**就地解码**（没有连续临时缓冲）失败时，把上一张壁纸恢复回去 ——
 * 否则桌面会停在一张写了一半的图上。
 */
bool _wpReloadSaved(uint8_t *dst, size_t bytes, int w, int h)
{
    char path[64];
    if (!_wpPickExisting(w, h, path, sizeof(path))) {
        return false;
    }
    WallpaperHeader hdr = {};
    if (!_wpReadHeader(path, hdr)) {
        return false;
    }
    if (hdr.magic != WP_MAGIC || hdr.version != WP_VERSION ||
            hdr.format != WP_FMT_RGB565_LE || hdr.data_size != bytes) {
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    void *iobuf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (iobuf != nullptr) {
        setvbuf(fp, (char *)iobuf, _IOFBF, 64 * 1024);
    }
    uint8_t discard[sizeof(WallpaperHeader)];
    bool ok = (fread(discard, 1, sizeof(discard), fp) == sizeof(discard)) &&
              (fread(dst, 1, bytes, fp) == bytes);
    fclose(fp);
    if (iobuf != nullptr) {
        heap_caps_free(iobuf);
    }
    if (!ok) {
        return false;
    }
    return (esp_rom_crc32_le(0, dst, (uint32_t)bytes) == hdr.crc32);
}

/* 给 _wpSave 用的 RAII 锁：早退分支多，手写 give 迟早在某条路上漏掉。 */
struct WpSaveGuard {
    bool locked = false;
    WpSaveGuard()
    {
        if (s_wp_save_io == nullptr) {
            s_wp_save_io = xSemaphoreCreateMutex();
        }
        if (s_wp_save_io != nullptr) {
            locked = (xSemaphoreTake(s_wp_save_io, portMAX_DELAY) == pdTRUE);
        }
    }
    ~WpSaveGuard()
    {
        if (locked) {
            xSemaphoreGive(s_wp_save_io);
        }
    }
};

/* 往指定分区写一份（dst/tmp 必须同一分区）。 */
bool _wpSaveOne(const char *fs_root, const char *dst, const char *tmp,
                const uint8_t *pixels, int w, int h)
{
    const int64_t t0 = esp_timer_get_time();
    const size_t bytes = (size_t)w * (size_t)h * 2u;

    /* 查容量用 esp_vfs_fat_info（IDF 的 newlib 没有 statvfs） */
    uint64_t total = 0;
    uint64_t freeb = 0;
    if (esp_vfs_fat_info(fs_root, &total, &freeb) == ESP_OK &&
            freeb < (uint64_t)bytes + 512u * 1024u) {
        ESP_UTILS_LOGW("壁纸不落盘：%s 只剩 %llu B，不够 %u B + 余量",
                       fs_root, (unsigned long long)freeb, (unsigned)bytes);
        return false;
    }

    FILE *fp = fopen(tmp, "wb");
    if (fp == nullptr) {
        ESP_UTILS_LOGE("壁纸临时文件打不开：%s", tmp);
        return false;
    }
    /* stdio 默认缓冲只有百来字节 ⇒ 2 MB 会变成几万次小写。 */
    void *iobuf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (iobuf != nullptr) {
        setvbuf(fp, (char *)iobuf, _IOFBF, 64 * 1024);
    }

    WallpaperHeader hdr = {};
    hdr.magic     = WP_MAGIC;
    hdr.version   = WP_VERSION;
    hdr.format    = WP_FMT_RGB565_LE;
    hdr.width     = (uint16_t)w;
    hdr.height    = (uint16_t)h;
    hdr.data_size = (uint32_t)bytes;
    hdr.crc32     = esp_rom_crc32_le(0, pixels, (uint32_t)bytes);

    bool ok = (fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr));
    ok = ok && (fwrite(pixels, 1, bytes, fp) == bytes);
    if (ok) {
        ok = (fflush(fp) == 0);
    }
    fclose(fp);
    if (iobuf != nullptr) {
        heap_caps_free(iobuf);
    }

    if (!ok) {
        ESP_UTILS_LOGE("壁纸写临时文件失败：%s（%u B）", tmp, (unsigned)bytes);
        remove(tmp);
        return false;
    }
    /* FAT 上 rename 覆盖已存在文件不一定成功 ⇒ 先 remove 目标。
     * 掉电窗口里最坏丢壁纸，不会留下半个文件被开机读出来。 */
    remove(dst);
    if (rename(tmp, dst) != 0) {
        ESP_UTILS_LOGE("壁纸 rename 失败：%s -> %s", tmp, dst);
        remove(tmp);
        return false;
    }
    ESP_UTILS_LOGI("壁纸已保存：%s（%dx%d，%u B，写盘 %lld ms）",
                   dst, w, h, (unsigned)bytes,
                   (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/*
 * 落盘入口：**SD 优先**，/data 只作退路。
 * 依据见上面壁纸落盘位置的实测：写 /data 的 2 MB 会让整个 SoC 卡 20 秒以上。
 */
bool _wpSave(const uint8_t *pixels, int w, int h)
{
    WpSaveGuard guard;      /* 同一时刻只允许一个写者，见 s_wp_save_io 的说明 */
    const WpPaths paths = _wpPaths(w, h);   /* 按方向分开存：横/竖屏互不覆盖 */

    if (_wpSdMounted() &&
            _wpSaveOne(BSP_SD_MOUNT_POINT, paths.sd, paths.sd_tmp, pixels, w, h)) {
        return true;
    }
    if (_wpSdMounted()) {
        ESP_UTILS_LOGW("壁纸写 SD 失败，退回 /data（flash 写会卡界面约 25 秒）");
    } else {
        ESP_UTILS_LOGW("SD 卡没挂上，壁纸退回 /data（flash 写会卡界面约 25 秒）");
    }
    return _wpSaveOne("/data", paths.data, paths.data_tmp, pixels, w, h);
}

/*
 * ---- 落盘任务 ----
 *
 * 为什么不能和解码共用一个 worker：2 MB 写 FAT 实测要 **21~26 秒**（含磨损均衡，
 *    约 80 KB/s）。共用 worker 时，用户紧接着点第二次会排在这次写盘后面 ⇒ 界面上的
 *    "Working..." 会卡二十多秒。拆成独立任务后，解码一完就放开按钮，写盘在后台继续。
 *
 * 只会保留**最新一张**待写：用户在写盘期间又设了一张，旧的那张还没写就直接丢掉
 *（它已经被新壁纸取代，写它没有意义）。
 *
 * 内存上限（回答"会不会堆积"）：常驻壁纸 1 块 2 MB（永久复用）；
 * 写盘最多 2 块临时 2 MB（一块正在写、一块排队），写完立刻 free。不会随设置次数增长。
 */
struct WpSaveSlot {
    uint8_t *buf = nullptr;
    int      w = 0;
    int      h = 0;
    bool     valid = false;
};
WpSaveSlot        s_save_slot;
SemaphoreHandle_t s_save_mutex = nullptr;
SemaphoreHandle_t s_save_sem = nullptr;
TaskHandle_t      s_save_task = nullptr;

void _wpSaveTaskEntry(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_save_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t *buf = nullptr;
        int w = 0;
        int h = 0;
        if (xSemaphoreTake(s_save_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_save_slot.valid) {
                buf = s_save_slot.buf;
                w = s_save_slot.w;
                h = s_save_slot.h;
                s_save_slot.buf = nullptr;
                s_save_slot.valid = false;
            }
            xSemaphoreGive(s_save_mutex);
        }
        if (buf != nullptr) {
            _wpSave(buf, w, h);
            heap_caps_free(buf);
            ESP_UTILS_LOGI("壁纸保存任务栈余量 %u B",
                           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
    }
}

/* 把 buf 的所有权交给保存任务。成功返回 true（调用方**不要**再 free buf）。 */
bool _wpQueueSave(uint8_t *buf, int w, int h)
{
    if (buf == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    if (s_save_sem == nullptr) {
        s_save_sem = xSemaphoreCreateBinary();
        s_save_mutex = xSemaphoreCreateMutex();
        if (s_save_sem == nullptr || s_save_mutex == nullptr) {
            return false;
        }
    }
    if (s_save_task == nullptr) {
        /* 钉在 core 0：LVGL 在 core 1，写 flash 时别去抢界面核。
         * 优先级 4 低于解码 worker（5）和 LVGL（10）。 */
        if (xTaskCreatePinnedToCore(_wpSaveTaskEntry, "album_wpsave", 8 * 1024,
                                    nullptr, 4, &s_save_task, 0) != pdPASS) {
            s_save_task = nullptr;
            return false;
        }
    }
    if (xSemaphoreTake(s_save_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (s_save_slot.valid && s_save_slot.buf != nullptr) {
        /* 上一张还没开始写就被新壁纸取代了 —— 丢掉，不写两遍。 */
        heap_caps_free(s_save_slot.buf);
    }
    s_save_slot.buf = buf;
    s_save_slot.w = w;
    s_save_slot.h = h;
    s_save_slot.valid = true;
    xSemaphoreGive(s_save_mutex);
    xSemaphoreGive(s_save_sem);
    return true;
}

/* WebP：libwebp 支持"先裁后缩"，一步直接产出屏幕尺寸（双向缩放都支持）。 */
bool _decodeWebpCover(const uint8_t *data, size_t len, int W, int H,
                      uint8_t *dst, const char *name)
{
    WebPBitstreamFeatures feat;
    if (WebPGetFeatures(data, len, &feat) != VP8_STATUS_OK) {
        ESP_UTILS_LOGE("'%s': 壁纸 WebP 头解析失败", name);
        return false;
    }
    const int64_t internal = (feat.format == 2)
                             ? (int64_t)feat.width * feat.height * 4
                             : (feat.has_alpha ? (int64_t)feat.width * feat.height : 0);
    if (internal > WEBP_INTERNAL_BUDGET) {
        ESP_UTILS_LOGE("'%s': 壁纸 WebP 内部缓冲 %lld MB 超预算",
                       name, (long long)(internal / (1024 * 1024)));
        return false;
    }

    int cw = 0, ch = 0, cx = 0, cy = 0;
    _coverRect(feat.width, feat.height, W, H, cw, ch, cx, cy);
    if (cw <= 0 || ch <= 0) {
        return false;
    }

    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) {
        return false;
    }
    cfg.output.colorspace         = MODE_RGB_565;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba        = dst;
    cfg.output.u.RGBA.stride      = W * 2;
    cfg.output.u.RGBA.size        = (size_t)W * (size_t)H * 2u;
    cfg.options.use_cropping      = 1;
    cfg.options.crop_left         = cx;
    cfg.options.crop_top          = cy;
    cfg.options.crop_width        = cw;
    cfg.options.crop_height       = ch;
    cfg.options.use_scaling       = 1;
    cfg.options.scaled_width      = W;
    cfg.options.scaled_height     = H;

    const int64_t t0 = esp_timer_get_time();
    const VP8StatusCode st = WebPDecode(data, len, &cfg);
    if (st != VP8_STATUS_OK) {
        ESP_UTILS_LOGE("'%s': 壁纸 WebPDecode 失败（%d）", name, (int)st);
        return false;
    }
    ESP_UTILS_LOGI("'%s': 壁纸 WebP 裁 %d,%d %dx%d -> %dx%d，%lld ms",
                   name, cx, cy, cw, ch, W, H,
                   (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

} // namespace

/* ------------------------------------------------------------------ */
/* 壁纸：开机恢复 / 后台任务 / 按钮                                     */
/* ------------------------------------------------------------------ */

bool Album::wallpaperRestore(lv_obj_t *main_screen)
{
    if (main_screen == nullptr) {
        return false;
    }
    const int w = (int)lv_display_get_horizontal_resolution(nullptr);
    const int h = (int)lv_display_get_vertical_resolution(nullptr);
    if (w <= 0 || h <= 0) {
        return false;
    }

    char path[64];
    if (!_wpPickExisting(w, h, path, sizeof(path))) {
        ESP_UTILS_LOGI("没有已保存的壁纸（%dx%d，SD / /data 都没有），桌面用默认背景", w, h);
        return false;
    }
    WallpaperHeader hdr = {};
    if (!_wpReadHeader(path, hdr)) {
        ESP_UTILS_LOGE("壁纸头读取失败：%s", path);
        return false;
    }
    const size_t bytes = (size_t)w * (size_t)h * 2u;
    if (hdr.magic != WP_MAGIC || hdr.version != WP_VERSION ||
            hdr.format != WP_FMT_RGB565_LE || hdr.data_size != bytes) {
        ESP_UTILS_LOGW("壁纸头不合法（magic/version/format/size），跳过");
        return false;
    }
    if ((int)hdr.width != w || (int)hdr.height != h) {
        /* 典型场景：以后切回竖屏 / 换了分辨率。**不删文件**，也不拿错尺寸去画。 */
        ESP_UTILS_LOGW("壁纸尺寸 %ux%u 与当前逻辑分辨率 %dx%d 不符，跳过",
                       (unsigned)hdr.width, (unsigned)hdr.height, w, h);
        return false;
    }

    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(64, bytes,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        ESP_UTILS_LOGE("壁纸缓冲 %u B 分配失败（PSRAM free %u KB / largest %u KB）",
                       (unsigned)bytes,
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }

    FILE *fp = fopen(path, "rb");
    void *iobuf = nullptr;
    bool ok = false;
    if (fp != nullptr) {
        iobuf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (iobuf != nullptr) {
            setvbuf(fp, (char *)iobuf, _IOFBF, 64 * 1024);
        }
        uint8_t discard[sizeof(WallpaperHeader)];
        ok = (fread(discard, 1, sizeof(discard), fp) == sizeof(discard)) &&
             (fread(buf, 1, bytes, fp) == bytes);
        fclose(fp);
    }
    if (iobuf != nullptr) {
        heap_caps_free(iobuf);
    }
    if (!ok) {
        ESP_UTILS_LOGE("壁纸文件读取失败");
        heap_caps_free(buf);
        return false;
    }
    if (esp_rom_crc32_le(0, buf, (uint32_t)bytes) != hdr.crc32) {
        ESP_UTILS_LOGE("壁纸 CRC 不符（写了一半 / 文件坏了），跳过");
        heap_caps_free(buf);
        return false;
    }

    uint8_t *old = s_wp.buf;
    s_wp.buf = buf;
    s_wp.cap = bytes;
    s_wp.w   = w;
    s_wp.h   = h;
    if (!_wpApply(main_screen)) {
        s_wp.buf = old;
        s_wp.cap = 0;
        s_wp.w = 0;
        s_wp.h = 0;
        heap_caps_free(buf);
        return false;
    }
    if (old != nullptr) {
        heap_caps_free(old);
    }

    ESP_UTILS_LOGI("壁纸已恢复：%s（%dx%d，%u B，PSRAM free %u KB / largest %u KB）",
                   path, w, h, (unsigned)bytes,
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                   (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    return true;
}

/* 点击那一刻（LVGL 任务）：只做快照 + 唤醒 worker，绝不做解码。 */
void Album::_startWallpaperJob(void)
{
    if (_wp_busy.exchange(true)) {
        return;                         /* 已经有一个在跑，别排队 */
    }
    if (_photo_count <= 0 || _full_index < 0 || _full_index >= _photo_count) {
        _wp_busy.store(false);
        return;
    }
    const Photo &p = _photos[_full_index];
    if (p.grid_note != nullptr || p.src_w <= 0 || p.src_h <= 0) {
        _wp_busy.store(false);
        _wpSetNote("Not ready", 0xFF8A80, 2000);
        return;
    }

    if (s_wp_sem == nullptr) {
        s_wp_sem = xSemaphoreCreateBinary();
        s_wp_job_mutex = xSemaphoreCreateMutex();
        s_wp_save_io = xSemaphoreCreateMutex();
        if (s_wp_sem == nullptr || s_wp_job_mutex == nullptr || s_wp_save_io == nullptr) {
            _wp_busy.store(false);
            _wpSetNote("No task", 0xFF8A80, 2000);
            return;
        }
    }
    if (s_wp_task == nullptr) {
        /* 12 KB 栈、优先级 5（低于 LVGL 的 10）：解码慢也不会把界面卡住。
         * 每轮结束会打印 high-water mark，不够再加。 */
        /* 钉在 core 0：LVGL 在 core 1（main.cpp 的 task_core_id=1），解码别去抢界面核。 */
        if (xTaskCreatePinnedToCore(_wallpaperTaskEntry, "album_wp", 12 * 1024, this, 5,
                                    &s_wp_task, 0) != pdPASS) {
            s_wp_task = nullptr;
            _wp_busy.store(false);
            _wpSetNote("No task", 0xFF8A80, 2000);
            return;
        }
    }

    if (xSemaphoreTake(s_wp_job_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        _wp_busy.store(false);
        _wpSetNote("Busy", 0xFF8A80, 2000);
        return;
    }
    _wp_job.valid     = true;
    _wp_job.from_file = p.from_file;
    _wp_job.is_webp   = p.is_webp;
    _wp_job.data      = p.data;
    _wp_job.data_len  = p.data_len;
    _wp_job.src_w     = p.src_w;
    _wp_job.src_h     = p.src_h;
    _wp_job.screen_w  = (int)lv_display_get_horizontal_resolution(nullptr);
    _wp_job.screen_h  = (int)lv_display_get_vertical_resolution(nullptr);
    if (p.from_file) {
        snprintf(_wp_job.path, sizeof(_wp_job.path), "%s", p.path);
    } else {
        _wp_job.path[0] = '\0';
    }
    xSemaphoreGive(s_wp_job_mutex);

    /* 万一 worker 一直拿不到 LVGL 锁，10 秒后按钮也会自己恢复（见 _onZoomSettle）。 */
    _wpSetNote("Working...", 0xC7CBD1, 10000);
    xSemaphoreGive(s_wp_sem);
}

void Album::_wallpaperTaskEntry(void *arg)
{
    Album *self = static_cast<Album *>(arg);
    for (;;) {
        if (xSemaphoreTake(s_wp_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        AlbumWallpaperJob job {};
        if (xSemaphoreTake(s_wp_job_mutex, portMAX_DELAY) == pdTRUE) {
            job = _wp_job;
            _wp_job.valid = false;
            xSemaphoreGive(s_wp_job_mutex);
        }
        if (job.valid && self != nullptr) {
            self->_runWallpaperJob(job);
            ESP_UTILS_LOGI("壁纸 worker 栈余量 %u B",
                           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
    }
}

void Album::_wpSetNote(const char *text, uint32_t color, uint32_t revert_ms)
{
    /* 调用方要么在 LVGL 任务上，要么持有 bsp_extra_display_lock()。
     * _forgetLvObjects() 会把指针清空 ⇒ 这里判空 + lv_obj_is_valid 双保险。 */
    if (_wp_label == nullptr || !lv_obj_is_valid(_wp_label)) {
        return;
    }
    lv_label_set_text(_wp_label, text);
    lv_obj_set_style_text_color(_wp_label, lv_color_hex(color), 0);
    _wp_revert_at = (revert_ms != 0) ? (lv_tick_get() + revert_ms) : 0;
}

void Album::_onSetWallpaperClicked(lv_event_t *e)
{
    if (_instance == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    _instance->_startWallpaperJob();
}

/* 真正的重活：解码 -> 应用 -> 落盘。跑在 worker 任务上。 */
void Album::_runWallpaperJob(const AlbumWallpaperJob &job)
{
    const int W = job.screen_w;
    const int H = job.screen_h;
    const size_t bytes = (size_t)W * (size_t)H * 2u;
    if (W <= 0 || H <= 0) {
        _wp_busy.store(false);
        return;
    }

    /*
     * 目标缓冲的选择（实测修正）：
     *
     *   - 已经有常驻壁纸缓冲时，**优先**用一块私有临时缓冲 —— 解码失败不会破坏当前壁纸。
     *   - 但 PSRAM 一碎片，2 MB 的**连续**临时缓冲就分不出来（实测第二次设壁纸时
     *     free 还有 4.9 MB，largest 已经 < 2 MB）。这时退回**直接解进常驻缓冲**：
     *     相册的屏幕盖在桌面之上，桌面此刻不会渲染，所以"写脏"不可见；
     *     万一解码失败，再从 /data 把上一张壁纸读回来（见下面的失败分支）。
     *   - 还没有常驻缓冲（本机第一次设壁纸）⇒ 直接分配常驻缓冲、解进去，
     *     省掉"临时 + 常驻"两份 2 MB 同时挂着。
     */
    const bool had_persistent = (s_wp.buf != nullptr && s_wp.cap >= bytes);
    bool used_persistent = false;
    bool persistent_fresh = false;
    uint8_t *tmp = nullptr;

    if (had_persistent) {
        tmp = (uint8_t *)heap_caps_aligned_alloc(16, bytes,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (tmp == nullptr) {
        if (!had_persistent) {
            uint8_t *nb = (uint8_t *)heap_caps_aligned_alloc(
                              16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb == nullptr) {
                ESP_UTILS_LOGE("壁纸缓冲 %u B 分配失败（PSRAM free %u KB / largest %u KB）",
                               (unsigned)bytes,
                               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
                _wp_busy.store(false);
                if (bsp_extra_display_lock(1000)) {
                    _wpSetNote("No memory", 0xFF8A80, 2500);
                    bsp_extra_display_unlock();
                }
                return;
            }
            s_wp.buf = nb;
            s_wp.cap = bytes;
            persistent_fresh = true;
        }
        tmp = s_wp.buf;
        used_persistent = true;
        ESP_UTILS_LOGW("壁纸就地解码：没有连续的 %u B 可用（free %u KB / largest %u KB）",
                       (unsigned)bytes,
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    }

    uint8_t *file_data = nullptr;
    size_t   file_len  = 0;
    const uint8_t *data = job.data;
    size_t len = job.data_len;
    if (job.from_file) {
        file_data = _readFileAlloc(job.path, &file_len);
        data = file_data;
        len = file_len;
    }
    const char *name = job.from_file ? job.path : "builtin";

    bool ok = false;
    if (data != nullptr && len > 0) {
        if (job.is_webp) {
            ok = _decodeWebpCover(data, len, W, H, tmp, name);
        } else {
            /*
             * JPEG：esp_new_jpeg 只能缩放、不能裁剪 ⇒
             *   ① 解一张"够用的"临时图（长边按 cover 需要算，再用 _safeEdge/_budgetEdge 收口）；
             *   ② 在解码结果里取居中的 8:5 区域；
             *   ③ 重采样到屏幕尺寸（恰好等大时直接逐行 memcpy）。
             */
            int cw = 0, ch = 0, cx = 0, cy = 0;
            _coverRect(job.src_w, job.src_h, W, H, cw, ch, cx, cy);
            if (cw > 0 && ch > 0) {
                const int src_long    = (job.src_w > job.src_h) ? job.src_w : job.src_h;
                const int cover_long  = (cw > ch) ? cw : ch;
                const int screen_long = (W > H) ? W : H;
                const int desired     = (cover_long < screen_long) ? cover_long : screen_long;

                int dec_edge = _alignJpeg((int)((int64_t)src_long * desired / cover_long));
                const int safe = _safeEdge(src_long);
                if (dec_edge > safe) {
                    dec_edge = safe;
                }
                dec_edge = _budgetEdge(job.src_w, job.src_h, dec_edge);
                if (dec_edge < JPEG_ALIGN) {
                    dec_edge = JPEG_ALIGN;
                }

                int dw = 0, dh = 0;
                _expectedOutSize(job.src_w, job.src_h, dec_edge, dw, dh);
                const size_t cap = (size_t)dw * (size_t)dh * 2u;
                uint8_t *dec = (uint8_t *)heap_caps_aligned_alloc(
                                   16, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (dec != nullptr) {
                    Photo p {};
                    p.name     = name;
                    p.data     = data;
                    p.data_len = len;
                    p.is_webp  = false;
                    int ow = 0, oh = 0, olen = 0;
                    if (_decodeTo(p, dec_edge, dec, cap, ow, oh, olen)) {
                        int dcw = 0, dch = 0, dcx = 0, dcy = 0;
                        _coverRect(ow, oh, W, H, dcw, dch, dcx, dcy);
                        if (dcw == W && dch == H) {
                            for (int y = 0; y < H; y++) {
                                memcpy(tmp + (size_t)y * (size_t)W * 2u,
                                       dec + ((size_t)(dcy + y) * (size_t)ow + (size_t)dcx) * 2u,
                                       (size_t)W * 2u);
                            }
                            ok = true;
                        } else if (_upscaleBilinear(
                                       dec + ((size_t)dcy * (size_t)ow + (size_t)dcx) * 2u,
                                       dcw, dch, ow * 2, tmp, W, H)) {
                            ok = true;
                        }
                    }
                    heap_caps_free(dec);
                } else {
                    ESP_UTILS_LOGE("壁纸 JPEG 临时图 %u B 分配失败（PSRAM free %u KB）",
                                   (unsigned)cap,
                                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
                }
            }
        }
    }
    if (file_data != nullptr) {
        heap_caps_free(file_data);
    }

    bool applied = false;
    if (ok) {
        /* 2 MB memcpy + 样式设置，放在锁里只有几十 ms；真正的解码/读盘已经在锁外做完。 */
        if (bsp_extra_display_lock(2000)) {
            auto *ctx = getSystemContext();
            lv_obj_t *main_screen = (ctx != nullptr) ?
                                    ctx->getDisplay().getMainScreenObject() : nullptr;
            if (main_screen != nullptr) {
                if (!used_persistent) {
                    /* tmp 是私有缓冲；把它拷进常驻缓冲。 */
                    if (s_wp.buf == nullptr || s_wp.cap < bytes) {
                        uint8_t *nb = (uint8_t *)heap_caps_aligned_alloc(
                                          64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (nb != nullptr) {
                            /* 先换指针再释放旧的：旧 dsc 在 _wpApply() 之前仍挂在主屏的样式上，
                             * 但我们持有锁、没有渲染在进行，换完再释放是安全的。 */
                            uint8_t *old = s_wp.buf;
                            s_wp.buf = nb;
                            s_wp.cap = bytes;
                            if (old != nullptr) {
                                heap_caps_free(old);
                            }
                        }
                    }
                    if (s_wp.buf != nullptr && s_wp.cap >= bytes) {
                        memcpy(s_wp.buf, tmp, bytes);
                    }
                }
                if (s_wp.buf != nullptr && s_wp.cap >= bytes) {
                    s_wp.w = W;
                    s_wp.h = H;
                    applied = _wpApply(main_screen);
                }
                _wpSetNote(applied ? "Wallpaper set" : "No memory",
                           applied ? 0x7CFFB2 : 0xFF8A80, 2500);
            }
            bsp_extra_display_unlock();
        } else {
            ESP_UTILS_LOGW("壁纸：%d ms 内没拿到 LVGL 锁，放弃本次应用", 2000);
        }
    } else {
        ESP_UTILS_LOGE("壁纸解码失败（%s）", name);

        /*
         * 只有"就地解码"才需要善后：常驻缓冲此刻可能是一张写了一半的图。
         *   ① 优先从 /data 把上一张读回来；
         *   ② 读不回来就把桌面壁纸撤掉（回到默认背景）—— 花屏比没壁纸更糟。
         * 第一次设壁纸时没有旧图，直接把刚分配的常驻缓冲还回去。
         */
        bool recovered = false;
        if (used_persistent && had_persistent) {
            recovered = _wpReloadSaved(s_wp.buf, bytes, W, H);
            if (recovered) {
                ESP_UTILS_LOGW("已把上一张壁纸从 /data 读回");
            } else {
                ESP_UTILS_LOGW("上一张壁纸也读不回来，撤掉桌面壁纸");
            }
        }
        if (used_persistent && persistent_fresh) {
            heap_caps_free(s_wp.buf);
            s_wp.buf = nullptr;
            s_wp.cap = 0;
        }

        if (bsp_extra_display_lock(1000)) {
            auto *ctx = getSystemContext();
            lv_obj_t *main_screen = (ctx != nullptr) ?
                                    ctx->getDisplay().getMainScreenObject() : nullptr;
            if (main_screen != nullptr && used_persistent && had_persistent) {
                if (recovered) {
                    lv_obj_invalidate(main_screen);
                } else {
                    lv_obj_set_style_bg_image_src(main_screen, nullptr, 0);
                    lv_obj_invalidate(main_screen);
                    s_wp.w = 0;
                    s_wp.h = 0;
                }
            }
            _wpSetNote("Failed", 0xFF8A80, 2500);
            bsp_extra_display_unlock();
        }
    }

    /* 应用结束就放开按钮；落盘还要几秒，但那是后台的事，不影响下一次点击。 */
    _wp_busy.store(false);

    if (applied) {
        if (used_persistent) {
            /* 就地解码：tmp 就是常驻缓冲，下一次设置会覆盖它 ⇒ 复制一份交给保存任务。
             * 复制不出来（碎片）就退回同步保存 —— 会挡住下一次设置，但至少能持久化。 */
            uint8_t *snap = (uint8_t *)heap_caps_aligned_alloc(
                                16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (snap != nullptr) {
                memcpy(snap, tmp, bytes);
                if (!_wpQueueSave(snap, W, H)) {
                    heap_caps_free(snap);
                    _wpSave(tmp, W, H);
                }
            } else {
                ESP_UTILS_LOGW("壁纸保存快照分配失败，改为同步保存（会挡住下一次设置）");
                _wpSave(tmp, W, H);
            }
        } else {
            if (_wpQueueSave(tmp, W, H)) {
                tmp = nullptr;          /* 所有权已交给保存任务 */
            } else {
                _wpSave(tmp, W, H);     /* 后台任务起不来 ⇒ 同步兜底 */
            }
        }
    }
    if (!used_persistent && tmp != nullptr) {
        heap_caps_free(tmp);
    }
}

/* ------------------------------------------------------------------ */
/* 事件                                                                */
/* ------------------------------------------------------------------ */

void Album::_onThumbClicked(lv_event_t *e)
{
    if (_instance == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    _instance->_openFull((int)(intptr_t)lv_event_get_user_data(e));
}

void Album::_onPrevClicked(lv_event_t *e)
{
    if (_instance == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    _instance->_openFull(_instance->_full_index - 1);
}

void Album::_onNextClicked(lv_event_t *e)
{
    if (_instance == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    _instance->_openFull(_instance->_full_index + 1);
}

void Album::_onBackToGridClicked(lv_event_t *e)
{
    if (_instance == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    _instance->_closeFull();
}

void Album::_onZoomChanged(lv_event_t *e)
{
    if (_instance == nullptr || lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    _instance->_applyZoom();
}

} // namespace esp_brookesia::apps




