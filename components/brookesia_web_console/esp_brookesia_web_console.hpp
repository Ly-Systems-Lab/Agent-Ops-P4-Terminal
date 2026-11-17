/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 网页控制台服务 —— 把 P4 变成手机浏览器能访问的小型 Web 服务端。
 *
 * 定位：这是「0→1 骨架」的第一块，**不是一次性的上传页**。
 *       当前挂 7 条内置路由（/ 、/api/status 、/api/files 、/api/upload 、/api/delete 、
 *       /api/download 、/api/mkdir），以后股票 / 天气 / 时钟各自调 addRoute() 往同一张路由表里挂。
 *
 * 这些接口**都带目标**：`root=data|sd`（默认 data）+ `dir=<相对子目录>`（默认空）。
 * 不带参数时行为与早期版本**完全一致** —— 老链接不会坏。
 *
 * 两个必须知道的前提（都实测过）：
 *   1. 出厂 Settings 开机时就做好了 esp_netif_init / esp_wifi_init，
 *      但 **esp_wifi_start() 只在用户进过「设置→WLAN」页才被调用** ⇒ 重启后不会自动联网。
 *      本服务负责把这一步补上（start() 内部幂等）。
 *   2. 出厂 WlanPage::close() 会 **注销它自己的断线重连处理器** ⇒ 退出设置页后一旦掉线
 *      就再也不会自动重连。本服务注册自己的重连逻辑兜住这个洞。
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"
#include "wear_levelling.h"

namespace esp_brookesia::services
{

class WebConsole
{
public:
    static WebConsole &getInstance();

    /* 幂等：把 16 MB 数据分区挂到 /data。
     * **独立于 start() 暴露**，因为它不只是"网页控制台"的事 ——
     * 音乐播放器、相册都要读 /data，所以 main.cpp 在**开机时**就调它。
     * （之前挂在 start() 里 ⇒ 不点开网页控制台，重启后 /data 是没挂的。） */
    bool ensureStorageMounted();

    /* 幂等：挂载数据分区 → 确保 WiFi 起来并连上 → 启动 HTTP 服务器。
     * 重复调用只做检查，不会重复挂载、也不会重复开服务器。
     * 返回值只表示「服务器起来了」；WiFi 没连上不算失败 —— 屏幕/网页上看得到状态。 */
    bool start();
    void stop();

    /* 只停 HTTP 服务器，**不动 /data 的挂载**。
     * 为什么不复用 `stop()`：它会 `esp_vfs_fat_spiflash_unmount_rw_wl("/data")`，
     * 而 `/data` 是**开机时 main.cpp 挂的**、Hub 的聊天历史也写在那上面。
     * 用户点「Stop Server」只是想让这个后台服务别占资源，
     * 不该顺手把别人的存储一起拆了（拆了 Hub 写历史会静默失败）。
     * 下次进 app 时 `start()` 会重新把服务器拉起来（幂等）。 */
    void stopServer();
    bool isRunning() const
    {
        return _server != nullptr;
    }

    struct Status {
        bool     running        = false;
        bool     wifi_connected = false;
        char     ip[20]         = {0};
        int      file_count     = 0;
        uint64_t fs_total       = 0;
        uint64_t fs_free        = 0;
        /* ---- SD 卡 ----
         * `sd_present == false` 表示**卡没挂上**（没插，或者开机后才插 —— 板子没有卡检测脚）。
         * 界面上必须把这两种情况分开说：**"空目录"和"卡不在"是两件事**，
         * 混在一起用户会以为卡上的东西被删了。 */
        bool     sd_present     = false;
        uint64_t sd_total       = 0;
        uint64_t sd_free        = 0;
    };
    /* with_storage = false 时**跳过**文件数与容量查询 ——
     * 那两项必须走文件系统 I/O（opendir/readdir/stat + 扫 FAT 空闲块），
     * 会和 httpd 的写盘抢 flash。实测这条噪声能把上传从 ~120 KB/s 拖到 ~35 KB/s（约 4 倍）。
     * ⇒ UI 每拍只取网络状态（纯 RAM），空闲时才取全量。 */
    Status getStatus(bool with_storage = true);

    /* 是否有会读写存储的操作（上传 / 删除）正在跑。
     * UI 靠它避开与这些操作争抢文件系统。 */
    bool isStorageBusy() const
    {
        return _storage_op.load(std::memory_order_relaxed) > 0;
    }

    /* 操作期间把计数 +1，析构时 -1。用 RAII，免得哪个早退分支漏减。 */
    class StorageGuard
    {
    public:
        explicit StorageGuard(WebConsole &console) : _console(console)
        {
            _console._storage_op.fetch_add(1, std::memory_order_relaxed);
        }
        ~StorageGuard()
        {
            _console._storage_op.fetch_sub(1, std::memory_order_relaxed);
        }
        StorageGuard(const StorageGuard &)             = delete;
        StorageGuard &operator=(const StorageGuard &)  = delete;

    private:
        WebConsole &_console;
    };

    /* ---- 扩展点：以后别的 app 想加自己的接口，在这里挂 ---- */
    using Handler = std::function<esp_err_t(httpd_req_t *)>;
    bool addRoute(const char *uri, httpd_method_t method, Handler handler);

    static constexpr const char *MOUNT_POINT = "/data";
    static constexpr const char *UPLOAD_DIR  = "/data/upload";
    static constexpr const char *PARTITION   = "userdata";

    /* 单文件上限。取 8 MB 是"手机照片绰绰有余、又不至于一次把 16 MB 分区塞爆"的折中。 */
    static constexpr size_t MAX_UPLOAD = 8u * 1024u * 1024u;

    /* SD 卡的上限**必须单独定**，而且它跟"卡有多大"无关 —— 它由**这条通道能做到多少**决定。
     *
     * 2026-10-14 实测：走 WiFi 传 SD，≤1 MB 能成功（941 KB/s），**≥2 MB 会把板子打崩重启**
     *   （esp-hosted 流式收包要一块**连续的内部 DMA 内存**，本板最大连续块只有 30~90 KB）。
     * 2026-10-15 用户拍板：**把上限下调到 1 MB** ——
     *   原来写的 512 MB 在 WiFi 这条通道上**根本够不着**，拦不住真正会出事的文件，形同虚设；
     *   提示是"劝"，闸才是"拦"：超过就当场 413 拒绝，而不是把板子打崩。
     *   ⇒ 想传大文件请走 U 盘模式（桌面「USB Drive」，实测写 3.85 MB/s / 读 8 MB/s）。
     * 别把这个数往大调 —— 它不是容量限制，是**崩溃阈值**下面的安全线。 */
    static constexpr size_t MAX_UPLOAD_SD = 1u * 1024u * 1024u;

private:
    WebConsole()                                = default;
    ~WebConsole()                               = default;
    WebConsole(const WebConsole &)              = delete;
    WebConsole &operator=(const WebConsole &)   = delete;

    bool _mountData();
    void _ensureWifi();
    bool _startServer();
    void _registerBuiltinRoutes();

    static void _onStaDisconnected(void *arg, esp_event_base_t base, int32_t id, void *data);
    static void _onGotIp(void *arg, esp_event_base_t base, int32_t id, void *data);

    httpd_handle_t _server = nullptr;
    wl_handle_t    _wl     = WL_INVALID_HANDLE;

    bool _wifi_hooks_registered = false;

    std::atomic<int> _storage_op{0};

    /* httpd_register_uri_handler() 会拷贝 httpd_uri_t 结构体，但 **不拷贝 uri 字符串本身**，
     * 所以字符串本体必须由我们持有到注销为止。
     * 用 deque 而不是 vector：deque 的 push_back 不会让已有元素的地址失效。 */
    std::deque<std::string> _owned_uris;
};

} // namespace esp_brookesia::services
