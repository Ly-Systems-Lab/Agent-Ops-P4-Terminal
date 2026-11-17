/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Svc:UsbStorage"
#include "esp_lib_utils.h"

#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   /* vTaskDelay */

/* FatFs 那三件套：卸卷 / 卸 diskio 驱动 / 卸 VFS 注册。
 * 都在 fatfs 组件的公共 include 目录里（diskio/ 与 src/）。 */
#include "ff.h"
#include "diskio.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_vfs_fat.h"

#include "bsp/esp-bsp.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"   /* tud_connect / tud_disconnect —— 主动让主机重新枚举 */

#include "brookesia_usb_storage.hpp"

using namespace esp_brookesia::services;

static const char *TAG = "UsbStorage";

/* PC 是否已经把我们枚举成一个"已配置"的 USB 设备。
 * 放在文件作用域是为了让 TinyUSB 的回调（C 风格函数指针）能写它，
 * 而不必把 tinyusb 的类型暴露到我们的公开头文件里。 */
static volatile bool s_host_attached = false;

extern "C" void usb_storage_tusb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_host_attached = true;
        ESP_LOGI(TAG, "USB host attached (rhport=%u)", event->rhport);
        break;
    case TINYUSB_EVENT_DETACHED:
        s_host_attached = false;
        ESP_LOGI(TAG, "USB host detached (rhport=%u)", event->rhport);
        break;
    default:
        break;
    }
}

UsbStorage &UsbStorage::getInstance(void)
{
    static UsbStorage instance;
    return instance;
}

bool UsbStorage::hostAttached(void) const
{
    return s_host_attached;
}

void UsbStorage::setError(const char *msg)
{
    snprintf(_err, sizeof(_err), "%s", msg);
    ESP_LOGW(TAG, "enter USB mode refused: %s", _err);
}

/*
 * 这个文件里有两条**绝对不能碰**的红线，都是一整轮实测/panic 换来的：
 *
 * 【红线 1】不许出现 sdmmc_host_deinit()（以及任何最终会调到它的封装）。
 *   原因：WiFi（esp-hosted）链路走**同一个 SDMMC 控制器的 slot 1**
 *   （esp_hosted_config.h:151 `H_SDMMC_HOST_SLOT = SDMMC_HOST_SLOT_1`；
 *    sdio_wrapper.c:287/302 调 sdmmc_host_init()/sdmmc_host_init_slot(slot1)），
 *   而 sdmmc_host_deinit() 是**遍历所有 slot** 全拆（sdmmc_host.c:931-935）。
 *   ⇒ 拆 SD 卡 slot 0 会把 WiFi 一起拆掉：串口被 `sdmmc_io … 0x108` /
 *     `H_SDIO_DRV: failed to read registers` 刷屏（实测 30 秒 6 MB），把界面任务饿死。
 *   而 IDF 的 esp_vfs_fat_sdcard_unmount() 最后一步正是 call_host_deinit() ⇒ **不能用**。
 *
 * 【红线 2】不许调用 esp_vfs_fat_sdmmc_mount()（重挂 SD 卡）。
 *   原因：releaseLocalMount() 是**温和**卸载，代价是 sdmmc 的 `s_ctx[]` 表
 *   （固定 FF_VOLUMES 个槽）会**漏掉一个**，而且没有任何公开 API 能还回去
 *   （只有 esp_vfs_fat_sdcard_unmount 会释放它，而那个在红线 1 里被禁了）。
 *   ⇒ 一旦再有人调 esp_vfs_fat_sdmmc_mount() 去要新槽，表满时就会
 *     `assert failed: esp_vfs_fat_sdmmc_mount vfs_fat_sdmmc.c:306 (ctx_id != FF_VOLUMES)`
 *     ⇒ **panic 重启**。实测：连进两次 U 盘模式就必崩（第二次的兜底重挂踩中）。
 *   ⇒ 所以**失败路径也不许重挂** —— 宁可明确告诉用户重启设备，也不去赌那一下。
 *     （想恢复 /sdcard 只有一条安全路：让 esp_tinyusb 以 MOUNT_APP 身份去挂，
 *       那走的是通用 vfs_fat 路径，不占 sdmmc 的槽。见 takeOverCard 的第 ③ 步。）
 *
 * 另外：esp_tinyusb 的 MSC storage 对象**内嵌** `CONFIG_TINYUSB_MSC_BUFSIZE` 那么大的
 *   收发缓冲，且用 `heap_caps_aligned_calloc(..., MALLOC_CAP_DMA)` 分配
 *   ⇒ 这个值等于"每次进 U 盘模式要一块**多大的连续内部 DMA 内存**"。
 *   实测（2026-10-15）：改成 32768 后，读速度 8 → 11 MB/s（+38%），写不变；
 *   但**碎片化后分配失败**（`Failed to allocate memory for MSC storage`），
 *   进 U 盘模式变得不可靠 ⇒ 回退到 8192。**别只看速度改这个值。**
 */

/* 把本地 /sdcard 的**视图**摘掉，但保留 SDMMC 主机与卡对象本身。
 * 三步骤与 esp_tinyusb 的 storage_sdmmc_unmount() + msc_storage_unmount() 完全一致。 */
static esp_err_t releaseLocalMount(void)
{
    sdmmc_card_t *card = bsp_sdcard;
    ESP_RETURN_ON_FALSE(card != nullptr, ESP_ERR_INVALID_STATE, TAG, "no SD card");

    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    ESP_RETURN_ON_FALSE(pdrv != 0xFF, ESP_ERR_INVALID_STATE, TAG,
                        "card is not registered with FatFs");

    char drv[3] = {(char)('0' + pdrv), ':', 0};

    /* ① 卸掉 FatFs 卷。这一步失败就意味着卷还挂在 FatFs 上，
     *    此时把卡交给 USB 主机 = 两边同时写同一块盘 ⇒ 直接放弃，别赌。 */
    FRESULT fr = f_mount(nullptr, drv, 0);
    ESP_RETURN_ON_FALSE(fr == FR_OK, ESP_FAIL, TAG, "f_mount(0) failed (%d)", (int)fr);

    /* ② 卸掉 diskio 驱动（宏，等价于 ff_diskio_register(pdrv, NULL)）。这一步不碰主机。 */
    ff_diskio_unregister(pdrv);

    /* ③ 注销 VFS 路径并释放 vfs_fat 的 ctx（esp_vfs_fat.c:236，纯簿记，不碰 FatFs/主机）。
     * 代价见上面【红线 2】：vfs_fat_sdmmc 层那个 ctx 槽会**漏掉**，每次进入漏一个。
     *       正常路径无所谓（之后由 esp_tinyusb 用通用 vfs_fat 路径管 /sdcard），
     *       但只要有人调 esp_vfs_fat_sdmmc_mount() 就可能踩 assert ⇒ 见红线 2。 */
    esp_err_t err = esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);
    ESP_RETURN_ON_FALSE(err == ESP_OK, err, TAG, "esp_vfs_fat_unregister_path failed");

    ESP_LOGI(TAG, "%s detached from the local VFS (SDMMC host and card kept alive)",
             BSP_SD_MOUNT_POINT);
    return ESP_OK;
}

/* 建 storage 实例。mount_point 决定它一建出来是归谁：
 *   MOUNT_APP —— esp_tinyusb 立刻把 /sdcard 挂回来（**通用 vfs_fat 路径，不占 sdmmc 槽**）；
 *   MOUNT_USB —— 只是把裸扇区暴露给主机，不挂 FATFS。 */
static esp_err_t createStorage(tinyusb_msc_mount_point_t mount_point,
                               tinyusb_msc_storage_handle_t *out)
{
    /* do_not_format = true 是**红线**：卡上挂不上文件系统时只报错，绝不格式化。 */
    tinyusb_msc_storage_config_t st_cfg = {};
    st_cfg.medium.card = bsp_sdcard;
    st_cfg.fat_fs.base_path = (char *)BSP_SD_MOUNT_POINT;
    st_cfg.fat_fs.config.max_files = 5;
    st_cfg.fat_fs.config.allocation_unit_size = 64 * 1024;
    st_cfg.fat_fs.config.format_if_mount_failed = false;
    st_cfg.fat_fs.do_not_format = true;
    st_cfg.mount_point = mount_point;

    return tinyusb_msc_new_storage_sdmmc(&st_cfg, out);
}

bool UsbStorage::takeOverCard(void)
{
    /* ① 先装 USB 栈。这一步完全不碰 SD ⇒ 失败的话 /sdcard 毫发无损。 */
    if (!_stack_installed) {
        /* 两个坑叠在一起，这个配置对象必须同时满足两条：
         *   ① **用官方宏整体构造**，别自己 `= {}` 再逐字段填 —— 零初始化会把 task.size 留成 0，
         *      tinyusb_driver_install 第一步的参数检查就挡下（`Task size can't be 0`），
         *      USB 栈压根没装起来、PC 侧什么都没有（很容易误判成线材问题）。
         *   ② **必须是 static**（静态存储期）—— esp_tinyusb **只存指针不拷贝**：
         *      `tinyusb_task_ctx_t.desc_cfg` 指向 `&config->descriptor`，而真正读它是在
         *      USB 任务里（tinyusb_task.c:71 → tinyusb_descriptors_set()）。
         *      放成局部变量 ⇒ 那时栈帧早没了 ⇒ 解引用残留 ⇒ `Load access fault` 崩溃重启。
         *      （症状：插不插线、插哪个口都一样崩 —— 因为它在 USB 任务启动时就跑。）
         *   宏给出：task 4096 字节 / 优先级 5、描述符留空走内置默认、
         *   端口 = TINYUSB_PORT_DEFAULT_HS（P4 上即 HS_0：这块板的口走 UTMI 高速 PHY）。 */
        static tinyusb_config_t tusb_cfg;
        tusb_cfg = TINYUSB_CONFIG_HIGH_SPEED(usb_storage_tusb_event_cb, nullptr);
        esp_err_t err = tinyusb_driver_install(&tusb_cfg);
        if (err != ESP_OK) {
            setError("USB stack install failed");
            ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
            return false;
        }

        tinyusb_msc_driver_config_t msc_cfg = {};
        /* 切换时机由我们（也就是用户点 app）决定，不要让 USB 插拔去自动改写谁的挂载点 */
        msc_cfg.user_flags.auto_mount_off = 1;
        err = tinyusb_msc_install_driver(&msc_cfg);
        if (err != ESP_OK) {
            setError("MSC driver install failed");
            ESP_LOGE(TAG, "tinyusb_msc_install_driver: %s", esp_err_to_name(err));
            tinyusb_driver_uninstall();
            return false;
        }
        _stack_installed = true;
    }

    /* ② 摘掉本地视图。**必须在暴露给主机之前做**：
     *    否则 PC 可能在 FatFs 还挂着的状态下写这张盘，两边的缓存各说各话。 */
    if (releaseLocalMount() != ESP_OK) {
        setError("cannot detach /sdcard");
        return false;
    }

    /* ③ 建 storage，**先以 APP 身份** —— 让 esp_tinyusb 把 /sdcard 挂回来。
     *    这样即使后面第 ④ 步失败，本地这块卡也是好的（而不是"摘掉了就回不来"）。
     * 这一步是**唯一会失败的地方**（内存碎片化时那个 DMA 连续分配会失败），
     *       失败后 /sdcard 处于"已摘掉"状态，而且**不能安全地重挂**（红线 2）
     *       ⇒ 只能明确告诉用户重启，别去赌。 */
    tinyusb_msc_storage_handle_t handle = nullptr;
    esp_err_t err = createStorage(TINYUSB_MSC_STORAGE_MOUNT_APP, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc(APP): %s", esp_err_to_name(err));
        if (err == ESP_ERR_NO_MEM) {
            /* 实测最可能的一种：storage 对象要一块 DMA 对齐连续内存（见文件头关于
             * CONFIG_TINYUSB_MSC_BUFSIZE 的说明），碎片化时拿不到。 */
            setError("out of DMA memory - restart the device");
        } else {
            setError("USB init failed - restart the device");
        }
        ESP_LOGE(TAG, "%s stays detached until restart", BSP_SD_MOUNT_POINT);
        return false;
    }
    _storage = handle;
    ESP_LOGI(TAG, "%s mounted locally again (owned by esp_tinyusb)", BSP_SD_MOUNT_POINT);

    /* ④ 再切给 USB 主机（温和：只摘 FATFS 视图，不碰卡、不碰主机） */
    err = tinyusb_msc_set_storage_mount_point(handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (err != ESP_OK) {
        /* 这一步失败的话 /sdcard 还在 APP 手上，本地照常可用 ⇒ 只报错，不慌 */
        setError("mount point switch failed");
        ESP_LOGE(TAG, "set_storage_mount_point(USB): %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

/* 强制主机重新枚举一次：断开 → 短延时 → 重连（等价于"用代码拔插一次那根线"）。
 *
 * 为什么必须做（2026-10-15 用户实测 + 串口日志定论）：Windows 一旦对这块盘执行过**弹出**，
 * 就会把该设备标记为"已弹出"；**只要设备不重新枚举，它就不再认这块介质** ——
 *   实测现象：弹出 → Leave → 再进，PC 再也不出盘（反复多次都一样）；**手动拔插一次线即恢复**。
 *   日志佐证：7 次进出循环里主机侧只 `attached` 过一次 —— 我们从不主动断开。
 *   （对照：不弹出行不行？用户实测 **不弹出时那条循环是完全正常的** ——
 *     说明"介质变化"这套机制在 Windows 上是work的，坏的只是"已弹出"的记忆。）
 *
 * 必须在**把卡交给 USB 主机之后**再调，这样主机是从零枚举、介质"一开始就在"，
 *    不必依赖它对"介质变化"的处理是否可靠。
 * 这也是"屏幕上那句'先弹出再离开'可以保留"的前提：弹出是**更安全**的做法
 *    （可移动盘若开了写缓存，不弹出就拔可能丢数据），所以我们不能靠"让用户别弹出"来绕。
 * 副作用：每次进入多约 0.25 秒，PC 会响一声设备连接音。 */
static void forceReattach(void)
{
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));
    tud_connect();
    vTaskDelay(pdMS_TO_TICKS(120));
}

bool UsbStorage::enterUsbMode(void)
{
    if (_active) {
        return true;
    }
    if (bsp_sdcard == nullptr) {
        setError("no SD card");
        return false;
    }

    if (_storage == nullptr) {
        if (!takeOverCard()) {
            return false;
        }
        ESP_LOGI(TAG, "USB drive mode ON (first time; SD card handed to the host)");
    } else {
        /* 之后每次进入：一次温和的挂载点切换（只卸本地 FATFS，不碰卡、不碰主机） */
        esp_err_t err = tinyusb_msc_set_storage_mount_point(
                             (tinyusb_msc_storage_handle_t)_storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
        if (err != ESP_OK) {
            setError("mount point switch failed");
            return false;
        }
        ESP_LOGI(TAG, "USB drive mode ON");
    }

    _active = true;
    s_host_attached = false;
    /* 不管第几次进，都让主机从零枚举一次（见上面注释；首次那一趟同样是幂等的重连） */
    forceReattach();
    return true;
}

bool UsbStorage::exitUsbMode(void)
{
    if (!_active) {
        return true;
    }
    if (_storage == nullptr) {
        _active = false;
        return true;
    }

    esp_err_t err = tinyusb_msc_set_storage_mount_point(
                         (tinyusb_msc_storage_handle_t)_storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (err != ESP_OK) {
        setError("mount point switch failed (back to app)");
        return false;
    }
    _active = false;
    ESP_LOGI(TAG, "USB drive mode OFF; %s is mounted locally again", BSP_SD_MOUNT_POINT);
    return true;
}
