set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS  ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    # xiaozhi-fonts：cbin_font_create()，加载 20 px 中文字库
    # cjson：聊天历史落盘
    # lvgl：UI
    # nvs_flash：hub_config.hpp（经由 brookesia_hub_service 的公共头）
    # lwip：TCP 探活用的 socket / getaddrinfo
    # brookesia_hub_service：常驻 MQTT 服务（MQTT 已不归 app 管，
    #                         证书嵌入也随它搬到了那个组件）
    REQUIRES brookesia_core xiaozhi-fonts cjson lvgl nvs_flash lwip brookesia_hub_service
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
