set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

# 显式列源文件（别 GLOB）：这个组件只有两个实现文件，GLOB 只会掩盖"新增了文件却没编进去"。
set(PROJ_SRCS
    ${PROJ_SRC}/brookesia_hub_service.cpp
    ${PROJ_SRC}/mqtt_transport.cpp
)

# ============================================================================
# MQTT 双向认证证书：**编译期嵌入**
# ----------------------------------------------------------------------------
# 源文件放工程根的 `secrets/` 目录（ca.crt / p4.crt / p4.key），该目录与
# `*.key` / `*.crt` 一起被 .gitignore 挡住 —— **私钥不入库**。
#
# 为什么用"每次都跑的 custom target"而不是 add_custom_command + DEPENDS：
# 证书文件**可能不存在**（干净的 clone），而 DEPENDS 一个不存在的文件会让构建直接失败。
# 生成脚本自己在"内容没变"时不重写文件，所以 mtime 稳定、不会引起无谓重编。
#
# `add_custom_target` / `add_dependencies` 在 IDF 的「依赖扫描」阶段（那一步用
#    `cmake -P` 跑，会把这个 CMakeLists 再 include 一次）**不可用**，会报
#    "add_custom_target command is not scriptable" ⇒ 必须用
#    `CMAKE_BUILD_EARLY_EXPANSION` 把这段挡在真正的配置阶段里。
# ============================================================================
# 证书目录在工程根下；生成脚本放在本组件的 tools/ 里，不跨层去引用仓根。
set(HUB_CERT_DIR ${PROJ_SRC}/../../secrets)
set(HUB_CERT_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(HUB_CERT_HDR ${HUB_CERT_GEN}/hub_certs.h)

file(MAKE_DIRECTORY ${HUB_CERT_GEN})

idf_component_register(
    SRCS  ${PROJ_SRCS}
    INCLUDE_DIRS "include" ${HUB_CERT_GEN}
    # brookesia_core：esp_lib_utils 的日志宏（ESP_UTILS_LOG_*）
    # mqtt：TLS 双向认证的 MQTT 客户端
    # cjson：回执报文解析 / 发送报文构造
    # nvs_flash：树莓派连接参数（hub_config.hpp 是公共头，故用 REQUIRES）
    # esp_timer：MQTT 的 30 s 堆水位采样
    # brookesia_net：等 WiFi 拿到 IP 再连（NetService）
    REQUIRES brookesia_core mqtt cjson nvs_flash esp_timer brookesia_net
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_target(hub_certs_header ALL
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1
                ${PYTHON} ${PROJ_SRC}/tools/gen_certs_header.py
                ${HUB_CERT_DIR} ${HUB_CERT_HDR}
        COMMENT "Embedding MQTT client certs (secrets/)"
        VERBATIM
    )
    add_dependencies(${COMPONENT_LIB} hub_certs_header)
endif()

set_source_files_properties(
    ${PROJ_SRCS}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
