set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS  ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core
    # esp32_p4_wifi6_touch_lcd_x：只为拿 BSP_SD_MOUNT_POINT（SD 卡的挂载点）这个宏，
    # 是编进本组件的头文件依赖，不需要传播出去 ⇒ 放 PRIV_REQUIRES。
    #
    #   libwebp   —— WebP 软件解码（我们的本地组件）
    #   bsp_extra —— wall paper worker 要 bsp_extra_display_lock()
    #   esp_rom   —— esp_rom_crc32_le()（壁纸文件校验）
    #   fatfs     —— esp_vfs_fat_info()（落盘前查 /data 余量）
    PRIV_REQUIRES esp_new_jpeg esp32_p4_wifi6_touch_lcd_x libwebp bsp_extra esp_rom fatfs)

# 把 JPEG 原始字节嵌进 app 固件，生成符号 _binary_<文件名>_start / _binary_<文件名>_end
# （名字只取文件名，photo1.jpg => _binary_photo1_jpg_start）。
# 注意：target_add_binary_data 一次只吃一个文件。
target_add_binary_data(${COMPONENT_LIB} "${PROJ_SRC}/assets/photo1.jpg" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${PROJ_SRC}/assets/photo2.jpg" BINARY)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
