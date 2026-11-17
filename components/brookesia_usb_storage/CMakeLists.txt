set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS  ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    # fatfs：esp_vfs_fat_sdcard_unmount()。brookesia_core：esp_lib_utils 的日志宏。
    REQUIRES brookesia_core fatfs
    # esp32_p4_wifi6_touch_lcd_x：BSP_SD_MOUNT_POINT 和全局 bsp_sdcard
    #               （这两个只在 .cpp 里用，不传播出去）
    # esp_tinyusb：USB 设备栈 + MSC；sdmmc / esp_driver_sdmmc：重建 SDMMC 主机与卡
    PRIV_REQUIRES esp32_p4_wifi6_touch_lcd_x esp_tinyusb sdmmc esp_driver_sdmmc
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
