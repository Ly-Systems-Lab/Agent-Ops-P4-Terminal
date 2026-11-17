/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 把板子上的 SD 卡当 U 盘交给 PC（USB 大容量存储 / MSC）。
 *
 * 为什么需要"模式"：SD 卡是块设备，USB MSC 也是块设备，两者**不能同时**访问。
 *   · 本地模式：卡由 FATFS 挂在 /sdcard，音乐播放器 / 相册 / 视频播放器在读；
 *   · U 盘模式：裸扇区交给 PC，本地必须先把 /sdcard 摘掉（否则 PC 绕过 FATFS
 *     写卡，本地那套文件系统缓存全错，卡会被写坏）。
 * 切换本身由 esp_tinyusb 的 storage mount point 完成：
 *   MOUNT_APP <-> MOUNT_USB 是**温和**的（只 f_mount(0) + 卸 diskio，不碰卡）。
 *
 * 首次进入时要把 BSP 挂的那份 /sdcard 收回来，这一步只能用"温和"做法：
 *    绝对不能用 IDF 的 esp_vfs_fat_sdcard_unmount() —— 它内部会 sdmmc_host_deinit()，
 *    而 WiFi（esp-hosted）链路走的是**同一个 SDMMC 控制器的 slot 1**，拆 slot 0 会把 WiFi 一起拆掉
 *    （2026-10-15 实测：串口被 sdmmc_io/H_SDIO_DRV 报错刷屏、界面被饿死）。
 *    详细原委写在 .cpp 顶部那段注释里。
 */
#pragma once

namespace esp_brookesia::services
{

class UsbStorage
{
public:
    static UsbStorage &getInstance(void);

    /** 本地 FATFS -> USB 主机。返回 false 时 lastError() 给出原因（ASCII，可直接上屏）。 */
    bool enterUsbMode(void);

    /** USB 主机 -> 本地 FATFS（把 /sdcard 挂回来）。 */
    bool exitUsbMode(void);

    bool active(void) const { return _active; }

    /** PC 有没有把我们枚举成一个已配置的 USB 设备（用于"拔线自动退出"）。 */
    bool hostAttached(void) const;

    /** 上一次失败原因。ASCII —— 会画到屏幕上，别写中文（LVGL 内置字体没有中文字形）。 */
    const char *lastError(void) const { return _err; }

private:
    UsbStorage(void) = default;
    ~UsbStorage(void) = default;
    UsbStorage(const UsbStorage &) = delete;
    UsbStorage &operator=(const UsbStorage &) = delete;

    /** 把卡从 BSP 的 VFS 手里收过来（含重建），并把 TinyUSB 设备栈装起来。只在首次进入时调用。 */
    bool takeOverCard(void);

    void setError(const char *msg);

    bool _active = false;
    bool _stack_installed = false;      // TinyUSB 设备栈 + MSC 驱动只装一次
    void *_storage = nullptr;           // tinyusb_msc_storage_handle_t（此处不引它的头，避免传播依赖）
    char _err[48] = {0};
};

} // namespace esp_brookesia::services
