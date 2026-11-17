set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS  ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    # esp_http_server / wear_levelling 进公共依赖：头文件里用到了 httpd_req_t 和 wl_handle_t，
    # 依赖本组件的 app 也得能看到这两个头。
    REQUIRES brookesia_core esp_http_server wear_levelling
    # esp32_p4_wifi6_touch_lcd_x：只为拿 BSP_SD_MOUNT_POINT（SD 卡的挂载点）这个宏。
    PRIV_REQUIRES esp_wifi esp_netif esp_event fatfs esp32_p4_wifi6_touch_lcd_x)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
