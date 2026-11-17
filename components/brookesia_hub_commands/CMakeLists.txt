set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

# 显式列源文件（别 GLOB）—— 这个组件只有一个实现文件，GLOB 只会掩盖
# "新增了文件却没编进去"这种链接期才炸的问题（新增组件目录或源文件不会自动触发 CMake 配置，必须先 reconfigure）。
idf_component_register(
    SRCS  ${PROJ_SRC}/brookesia_hub_commands.cpp
    INCLUDE_DIRS "include"
    # brookesia_core   ：Context::sendAppEvent / Manager::getInstalledApp
    # brookesia_hub_service：命令的来源（HubService）与 ack 的出口
    # MusicPlayer      ：music 命令要调它的 playFirst()
    # lvgl             ：命令队列靠一个 lv_timer 在 LVGL 任务里消费
    REQUIRES brookesia_core brookesia_hub_service MusicPlayer lvgl
)

set_source_files_properties(
    ${PROJ_SRC}/brookesia_hub_commands.cpp
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)