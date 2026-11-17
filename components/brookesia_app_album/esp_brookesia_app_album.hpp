/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 相册 app（本项目的第一个真功能；还会列出网页上传到 /data/upload 的图片）。
 *
 * 设计要点（为什么这么写）：
 *   - 内嵌的示例图以**原始 JPEG 字节**编进 app 二进制（CMake 的 target_add_binary_data），
 *     不碰 storage 分区，保住"我们只刷过 app 分区"这条已验证的不变量。
 *   - 上传的图**只读文件、不落盘任何缓存**（早期试过预生成 .thumbs，被判回退，别再捡回来）。
 *   - 运行时用 esp_new_jpeg 解码；**它在"目标输出尺寸接近源尺寸"时会静默截断**，
 *     所以解码尺寸一律经 _safeEdge() 收口（见 .cpp 里 JPEG_SAFE_GAP_* 的说明）。
 *   - 位图**按需解码**、只存内存：网格用一张小图（长边 GRID_EDGE），大图页用基准位图 +
 *     按倍率重解的明细图。平移靠 LVGL 的 memcpy 快路径，所以显示一律 1:1。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps
{

/*
 * 壁纸 worker 的"任务快照"。
 *
 * 为什么放在类外：它要作为 `static` 数据成员在 .cpp 里定义，
 * 而**类内私有类型**没法在命名空间作用域写出那个定义（访问控制会拒绝）。
 *
 * 为什么是快照而不是 `Photo&`：`_releaseUploads()` 会在重进 app 时把条目
 * memset/free 掉，而 worker 可能还在跑。这里只拷贝当时需要的那几项。
 */
struct AlbumWallpaperJob {
    bool           valid;
    bool           from_file;
    bool           is_webp;
    char           path[192];       /* 与 Album::ALBUM_PATH_MAX 一致 */
    const uint8_t *data;            /* 内嵌图：指向 flash，生命周期与固件相同 */
    size_t         data_len;
    int            src_w;
    int            src_h;
    int            screen_w;        /* 点击那一刻的逻辑分辨率 */
    int            screen_h;
};

class Album : public systems::phone::App
{
public:
    static Album *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~Album();

    /*
     * 开机时把上次保存的壁纸读回来并挂到桌面上。
     * 必须在 `phone->begin()` 之后、**持有 LVGL 锁**时调用（只做一次 I/O + 一次样式设置）。
     * 文件不存在 / 尺寸与当前逻辑分辨率不符 / CRC 不对 ⇒ 返回 false，桌面保持默认背景。
     */
    static bool wallpaperRestore(lv_obj_t *main_screen);

protected:
    Album(bool use_status_bar, bool use_navigation_bar);

    // 基类里 run() / back() 是纯虚，必须实现
    bool run(void) override;
    bool back(void) override;

    /*
     * **生命周期一共有四个出口，大缓冲必须**每一个**都放**：
     *   · `back()`        —— 左/右边缘横滑（我们 notifyCoreClosed）
     *   · `close()`       —— 框架关 app（**包括开别的 app**、最近任务里划掉、超上限自动关）
     *   · `pause()`       —— 下方手势条上滑 = 进最近任务；屏幕**不销毁**，只是被藏起来
     *   · 析构            —— 基本等不到（框架只会 CLOSED，不 delete 实例）
     * 只写 `back()` 是不够的：实测用户从大图页点 Back 按钮回网格、
     * 再进 VideoPlayer，相册那 6 MB 还挂着 ⇒ 视频解不出帧（黑屏有声音）、
     * 相册自己的缩略图/放大也分配失败。**"看起来就是退出"的那条路不是唯一的退出路。**
     */
    bool close(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    /* 文件名缓冲长度。上传侧把名字限制在 96 字符内，这里对齐。
     * 名字带 ALBUM_ 前缀是必须的：NAME_MAX / PATH_MAX 是 libc 已经定义的宏。 */
    static constexpr size_t ALBUM_NAME_MAX = 96;
    static constexpr size_t ALBUM_PATH_MAX = 192;

    /**
     * 一张照片。三种身份要分清，别混：
     *   - **网格小图**（grid_*）：网格格子里画的那张。内嵌图直接借用大图位图（只有 2 张，不心疼）；
     *     上传图**按需解一张小图**，解之前是占位格。
     *   - **大图页基准位图**（100% 时显示的那张）：
     *       · 内嵌图 —— 常驻（pixels/w/h/dsc），加载时解一次、只解一次；
     *       · 上传图 —— **不常驻**！打开大图页时才解进 Album 的共享槽（_base_buf/_base_dsc），
     *                   退出即弃。否则每张常驻一份大位图，PSRAM 顶不住。
     *       所以对上传图，pixels/w/h/dsc 是空的，真正要用的是 _curBaseDsc()/_curBaseW() 那组访问器。
     *   - **按倍率重解的明细图**：在 Album 的 _detail_buf 里，一次只留一张。
     */
    struct Photo {
        /* ---- 来源 ---- */
        const char     *name;       /* 显示名；上传图指向 name_buf */
        bool            from_file;  /* true = 数据分区上的文件 */
        const uint8_t  *data;       /* from_file=false 时有效：内嵌在固件里的**编码字节**（JPEG 或 WebP） */
        size_t          data_len;
        char            path[ALBUM_PATH_MAX];
        char            name_buf[ALBUM_NAME_MAX];

        /* ---- 元信息（只读文件头探出来，不解码） ---- */
        int             src_w;
        int             src_h;
        int             src_max;    /* 源长边 —— 决定这张图最大能放到多少倍 */
        uint8_t         sof;        /* JPEG 的 SOF 标记：0xC0 = 基线式（能解），0xC2 = 渐进式（板上解不了） */
        bool            baseline;

        /* ---- WebP。只有 is_webp 为真时下面这几项才有意义 ---- */
        bool            is_webp;
        uint8_t         webp_format;    /* 1 = 有损 VP8，2 = 无损 VP8L，0 = 没探到图像块 */
        bool            webp_alpha;
        bool            webp_animation;

        /* ---- 网格小图 ---- */
        uint8_t        *grid_px;    /* PSRAM；由本类持有 */
        int             grid_w;
        int             grid_h;
        lv_image_dsc_t  grid_dsc;
        /* 画不出图时写在格子上的原因（占位格）。nullptr = 正常。
         * 对上传图，"还没轮到解"和"解不了"是两回事：前者两者都为 nullptr，后者 grid_note 非空。 */
        const char     *grid_note;
        char            note_buf[96];

        /* ---- 大图页基准位图（只有内嵌图用；见上面的说明） ---- */
        uint8_t        *pixels;
        int             w;
        int             h;
        lv_image_dsc_t  dsc;
    };

    /* 内嵌 2 张 + 上传的。上传图在网格里每张只占 GRID_EDGE² × 2 字节（约 100 KB），
     * 所以这个上限主要由"扫目录时别把内存吃光"决定。 */
    static constexpr int MAX_PHOTOS      = 24;
    /* 网格小图的长边。要**正好等于格子里的图片框边长**，这样 CONTAIN 不缩放、绘制走 memcpy。
     * 必须 16 的倍数（JPEG_ALIGN）。 */
    static constexpr int GRID_EDGE       = 224;
    /* 上传目录（与网页控制台的 WebConsole::UPLOAD_DIR 一致，那边是权威定义）。 */
    static constexpr const char *UPLOAD_DIR = "/data/upload";
    /* 基准解码长边的**兜底值**（可视区拿不到时才用）。正常情况下走 _baseEdgeFor()。 */
    static constexpr int DECODE_MAX_EDGE = 720;

    /*
     * 缩放策略（2026-10-12 修订，解决"放大后平移完全不跟手"）：
     *   LVGL 里只有 scale==256 才走 memcpy 快路径，只要缩放就掉进逐像素双线性重采样。
     *   ⇒ 所以放大后**不让 LVGL 缩放**，而是按倍率**重新解码**一张 1:1 的位图来显示。
     *   好处：平移永远走快路径（丝滑）+ 画质用上源图的真实像素（不再"720 吹大"）。
     */
    static constexpr int ZOOM_MIN  = 100;   /* 百分比，100% = 基准尺寸，1:1 */
    static constexpr int ZOOM_MAX  = 200;   /* 硬上限。**不按源分辨率收窄** —— 见 .cpp 里的说明 */

    /*
     * "拖动闸"：调完倍率之后的一段时间里**先不让拖动**，分三档（用户 2026-10-13 指定）。
     *
     * 这是**体感补丁，不是根治** —— 根因是"停手后必须重解一张 1:1 位图"这个机制本身；
     * 那段时间里显示的仍是 LVGL 缩放的预览图，拖起来必然卡。用户实测：
     * "放大后立刻拖动就是卡顿，等跳出缓冲期后再拖就正常跟手"，倍率越高这段越长。
     * 换上清晰图（或超时）就立刻放开，所以正常情况下感觉不到闸的存在。
     * 用户明确认可这个取舍：相册不是本项目的核心功能，不值得为它过度深挖。
     *
     * 这两个阈值（130% / 150%）和两个时长都是**按手感定的**，不是量出来的 ——
     *    要调就改这四个数，别去动别的逻辑。
     */
    static constexpr int      PAN_GATE_MID_Z   = 130;    /* ≤ 这个倍率：不设闸 */
    static constexpr int      PAN_GATE_HIGH_Z  = 150;    /* ≤ 这个倍率：中档；再往上：高档 */
    static constexpr uint32_t PAN_GATE_MID_MS  = 2000;
    static constexpr uint32_t PAN_GATE_HIGH_MS = 5000;
    /*
     * 清晰图换上之后**再等这么久才开闸**（中档 / 高档各一个值）。
     *
     * 原来的做法是"清晰图一换上立刻开闸"，用户实测发现有问题：
     *    "防抖解开的一瞬间我正在滑动，还是会卡顿不跟手（尤其 150%+）"——
     *    手指按着的状态下去掉/加回 SCROLLABLE，LVGL 会把"按下去以来的位移"一次性算进去，
     *    看起来就是猛地一顿。多等一会儿，让换图那一下的重绘彻底落定。
     *
     * 这里的目标**不是根治**，是**用时间把出错的概率压下去**（用户明确的取舍：
     *    "压到 90% 就是胜利，而不是容忍问题出现再根治 —— 相册不是核心功能")。
     *    实测 150%+ 原来 1 秒挡不住（每次"清晰图换上"的时刻本身是抖动的），
     *    所以那一档加长到 2 秒；≤150% 那两个档已经是 90%+ 不出错，**别去动它们**。
     */
    static constexpr uint32_t PAN_GATE_SETTLE_MS      = 1000;   /* 130%~150% */
    static constexpr uint32_t PAN_GATE_SETTLE_HIGH_MS = 2000;   /* >150% */

    /* ---- 加载 ---- */
    bool _loadBuiltins(void);           /* 编进固件的那 2 张，只做一次 */
    bool _scanDir(const char *dir);     /* 扫一个目录（UPLOAD_DIR / SD 卡根）：只读文件头，不解码 */
    void _releaseUploads(void);         /* 放掉上次扫到的上传条目（内嵌的保留） */
    bool _decodeInto(Photo &p);         /* 内嵌图：解出常驻的基准位图 */
    /* ---- 解码 ---- */
    /* 把 p 的 JPEG 解码到 target_edge（长边、16 的倍数），输出写进调用方给的 buf。
     * 对上传图会先把整个文件读进 PSRAM 再解 —— 所以这才是"通用"入口。 */
    bool _decodeAt(const Photo &p, int target_edge, uint8_t *buf, size_t buf_cap,
                   int &out_w, int &out_h, int &out_len);
    /* 上面那个的"从内存解"版本（内嵌图直接走它，_decodeAt 也转调它）。 */
    bool _decodeTo(const Photo &p, int target_edge, uint8_t *buf, size_t buf_size,
                   int &out_w, int &out_h, int &out_len);
    /* "100%" 对应的解码长边：等比缩到刚好装进可视区（contain），**但不放大**。 */
    int  _baseEdgeFor(int src_w, int src_h);
    /* ---- 大图页基准位图（内嵌 / 上传两种来源统一从这组访问器取） ---- */
    bool _ensureBase(int index);        /* 上传图：需要时把它解进共享槽 */
    const lv_image_dsc_t *_curBaseDsc(void) const;
    int  _curBaseW(void) const;
    int  _curBaseH(void) const;
    int  _curBaseLong(void) const;
    /* ---- 大图页缩放 ---- */
    int  _detailTarget(void);   /* 当前倍率对应的**位图**解码长边 */
    void _syncDetail(void);     /* 停手后：挑出最好的位图 + 必要时放大一次，切到 1:1 显示 */
    /* 按 target 重解一张明细位图（带失败记忆）。成功返回它的 dsc，失败/刚失败过返回 nullptr。 */
    const lv_image_dsc_t *_ensureDetail(int target);
    /* 把 src 一次性放大到 disp_w×disp_h。超预算或分配失败返回 nullptr（由调用方退回 LVGL 缩放）。 */
    uint8_t *_ensureScaled(const uint8_t *src, int sw, int sh, int sstride,
                           int disp_w, int disp_h, size_t *out_len);
    void _layoutFull(int w, int h);   /* 设尺寸并摆位置/居中（宽高由调用方给，不现查） */
    /* 拖动闸（见 PAN_GATE_* 的说明）：关闸 = 这段时间不许拖；开闸 = 放行。
     * _closePanGate(0) 表示"这一档不需要闸"，会顺手开闸。
     * _markDetailReady() 由 _syncDetail 在"清晰图换上了"时调，**不是**立刻开闸，
     * 而是从这里开始再倒计时 PAN_GATE_SETTLE_MS。 */
    void _closePanGate(uint32_t ms, uint32_t settle_ms);
    void _openPanGate(void);
    void _markDetailReady(void);

    /* 文件名显示上限：取**第一张图名字的长度**（用户要求"全部以图片1为基准"）。
     * 超长的上传名（手机截图那种 50 个字符的）会把底部那条挤爆，必须截。 */
    int _nameLimit(void) const;

    /* ---- 网格 ---- */
    /* 网格里该用哪份位图：内嵌图借它自己的大图（只有 2 张，不心疼），上传图用它自己的小图。 */
    static const lv_image_dsc_t *_gridDsc(const Photo &p);
    void _buildGrid(void);
    void _fillNextThumb(void);          /* 懒解码：每拍补一张网格小图 */
    void _syncThumbCells(void);         /* 把小图挂到对应格子上 */

    void _openFull(int index);
    void _closeFull(void);
    void _applyZoom(void);

    /* 忘掉这一轮建的全部 lv 对象指针。**run() 和 back() 都要调**：
     * 框架销毁默认屏幕之后这些指针立刻变野，而两个定时器是跨 app 生命周期常驻的、
     * 还会继续跑 —— 在野指针上调 lv_image_set_src() 就是用后释放。 */
    void _forgetLvObjects(void);
    /* 放掉三块大缓冲（明细 / 放大 / 上传图的基准）。退出大图页时就放 ——
     * 一张手机原图在高倍率下要十几 MB，全留着会在别处撑爆内存（见 .cpp 里的说明）。 */
    void _releaseBigBuffers(void);

    /* 大图页那套东西的"软收摊"：断开 _full_img 的引用 → 记账清零 → 放三块大缓冲。
     * **不碰 lv 指针**（对象还活着），所以 `_closeFull()` 和 `pause()` 都能调。 */
    void _releaseViewer(void);
    /* `back()` / `close()` 共用：屏幕**即将被框架销毁**，所以连 lv 指针一起忘掉。 */
    void _teardownForScreenLoss(void);

    static void _onThumbClicked(lv_event_t *e);
    static void _onPrevClicked(lv_event_t *e);
    static void _onNextClicked(lv_event_t *e);
    static void _onBackToGridClicked(lv_event_t *e);
    static void _onZoomChanged(lv_event_t *e);
    static void _onZoomSettle(lv_timer_t *t);
    static void _onThumbTick(lv_timer_t *t);

    /* ---- 壁纸 ----
     * 解码很慢（几十万像素 + 可能读一个几 MB 的文件），**绝不能**放在 LVGL 任务里做。
     * 点击只做一次"快照 + 唤醒后台任务"，真正的解码/落盘都在 worker 里。
     * 快照类型见类外的 AlbumWallpaperJob（为什么放类外见那里的注释）。 */
    static AlbumWallpaperJob _wp_job;

    void _startWallpaperJob(void);
    void _runWallpaperJob(const AlbumWallpaperJob &job);
    void _wpSetNote(const char *text, uint32_t color, uint32_t revert_ms);
    static void _onSetWallpaperClicked(lv_event_t *e);
    static void _wallpaperTaskEntry(void *arg);

    static Album *_instance;

    Photo     _photos[MAX_PHOTOS] {};
    int       _photo_count = 0;
    int       _builtin_count = 0;  /* 前这么多个是内嵌图；它们只加载一次 */
    int       _failed_count = 0;   /* 画不出来的张数（画出来，别让失败悄悄发生） */
    int       _truncated = 0;      /* 因为超过 MAX_PHOTOS 被丢掉的张数 —— 必须告警 */

    lv_obj_t *_grid = nullptr;
    lv_obj_t *_fullview = nullptr;
    lv_obj_t *_zoom_view = nullptr;     /* 可滚动的视口，图片是它的子对象 */
    lv_obj_t *_zoom_slider = nullptr;   /* 侧边竖向倍率滑动条 */
    lv_obj_t *_zoom_label = nullptr;    /* 显示当前倍率 */
    lv_obj_t *_full_img = nullptr;
    lv_obj_t *_full_caption = nullptr;
    int       _full_index = 0;

    /* 网格每格的图片对象 / 占位文字对象。
     * 框架退出 app 会销毁默认屏幕 ⇒ 这些指针每次 run() 都要清空，否则就是用后释放。 */
    lv_obj_t *_cell_img[MAX_PHOTOS] {};
    lv_obj_t *_cell_note[MAX_PHOTOS] {};
    lv_timer_t *_thumb_timer = nullptr; /* 懒解码网格小图；补完自己停 */

    /* 1:1 明细位图：按倍率重新解码的结果，显示时不做任何缩放 */
    lv_timer_t    *_zoom_timer = nullptr;   /* 防抖：停手 250ms 后才重解码 */
    int            _zoom_seen = -1;         /* 上一拍看到的滑动条值 —— 用来判断"手还在动" */
    int            _view_w = 0;             /* _zoom_view 的视口尺寸，创建时记下 */
    int            _view_h = 0;             /* 不要现查 —— 刚建好的对象 lv_obj_get_* 可能是 0 */
    uint8_t       *_detail_buf = nullptr;   /* PSRAM；按需要分配，用大不缩小 */
    size_t         _detail_buf_size = 0;
    /* 放大缓冲：屏幕上要的尺寸超过"解码器敢给"的上限时，我们**一次性**把明细位图放大到这里，
     * 之后按 1:1 显示，平移才回得到 LVGL 的 memcpy 快路径（见 .cpp 里 _upscaleBilinear 的说明）。 */
    uint8_t       *_scaled_buf = nullptr;
    size_t         _scaled_buf_size = 0;
    lv_image_dsc_t _detail_dsc {};
    int            _detail_edge = 0;        /* _detail_dsc **实际**解码出来的长边；0 = 未就绪 */
    int            _detail_photo = -1;      /* _detail_dsc 属于哪张图 */
    /* 这张明细位图是**按哪个"理想目标长边"**解出来的。
     * 平时 `_detail_want == _detail_edge`；内存不够时会**降档**（`_detail_edge` 更小，
     * 但 `_detail_want` 仍记理想值）⇒ 下一次同样的请求直接命中缓存，
     * 不会每 250 ms 重撞一次分配（那是日志刷爆 + 画面永远卡在模糊期的老病根）。 */
    int            _detail_want = 0;
    /* 当前 **_full_img 上挂的**是哪一张、显示长边多大。
     * 它是"要不要干重活"的守卫：_syncDetail 每 250ms 跑一次，没有它就会反复重解/重放大。
     * 放大出来的那张单独用一个 dsc（_shown_dsc），免得和"解码结果"那个 dsc 混在一起 ——
     * 两者尺寸不一样，混用会让"已经解好了"的判断跟着错。 */
    lv_image_dsc_t _shown_dsc {};
    int            _shown_photo = -1;
    int            _shown_long = 0;
    /* 进后台（pause）时如果正停在大图页，记下那是第几张 ⇒ resume 时把用户放回原处。
     * -1 = 进后台时不在大图页（或者不需要恢复）。只记索引、不记倍率：
     * 大缓冲已经放掉了，回来必须重解一次基准图，倍率本来就会复位（这点代价是明知的）。 */
    int            _resume_full = -1;
    /* 拖动闸状态（见 PAN_GATE_*） */
    bool           _pan_gated = false;
    uint32_t       _pan_started = 0;
    uint32_t       _pan_ms = 0;         /* 本次闸的档位时长（上限） */
    uint32_t       _pan_settle_ms = 0;  /* 本次闸"清晰图换上后再等"的时长 */
    uint32_t       _pan_ready_at = 0;   /* 清晰图换上的时刻；0 = 还没换上 */
    /* "这张图的这个目标尺寸刚解失败过" —— 防抖定时器每 250ms 跑一次，
     * 没有这个记忆的话失败会每 250ms 重撞一次：日志被刷爆、CPU 白烧，
     * 画面还会一直卡在"模糊"的预览态（用户实测报过）。 */
    int            _detail_fail_photo  = -1;
    int            _detail_fail_target = -1;

    /* 上传图的大图页基准位图 —— **共享槽，一次只留一张**（退出大图页即弃）。
     * 每张常驻一份的话，1440 的图一张就是 4 MB，几张就把 PSRAM 吃穿了。 */
    uint8_t       *_base_buf = nullptr;
    size_t         _base_buf_size = 0;
    lv_image_dsc_t _base_dsc {};
    int            _base_w = 0;
    int            _base_h = 0;
    int            _base_photo = -1;        /* _base_dsc 属于哪张上传图；-1 = 无 */

    /* ---- 壁纸状态 ----
     * 这两个指针随默认屏幕一起销毁 ⇒ _forgetLvObjects() 必须清空，否则就是用后释放。 */
    lv_obj_t *_wp_btn = nullptr;
    lv_obj_t *_wp_label = nullptr;
    /* 状态文字恢复成 "Set Wallpaper" 的时刻（lv_tick_get() 的时间轴）；0 = 不用恢复。
     * 复用已有的 250 ms _onZoomSettle 定时器来检查，**不新建 lv_timer**（框架会回收 run() 期间建的 lv_timer，自己再管一遍就会删两次）。 */
    uint32_t _wp_revert_at = 0;
    std::atomic<bool> _wp_busy{false};
};

} // namespace esp_brookesia::apps
