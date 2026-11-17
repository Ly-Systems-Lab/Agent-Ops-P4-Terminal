# Google libwebp 1.5.0 —— **只保留解码器**（dec / dsp / utils 的纯 C 部分）。
#
# 为什么只留解码器：相册只需要把 WebP 解成 RGB565，编码器（enc/）、
# 动画 demux、以及所有 SIMD（sse2/sse41/neon/msa/mips）都不需要；
# 后两者在 riscv32 上根本编不过。
#
# 🚨 三个编译宏都是**必须**的，别删：
#   WEBP_SWAP_16BIT_CSP=1
#       libwebp 的 MODE_RGB_565 默认按 [rg, gb] 写（**大端**字节序），
#       而 LVGL 在 P4（小端）上按 uint16 小端解释 RGB565。
#       不 swap 的颜色会红绿蓝全错 —— 依据 src/dsp/yuv.h:100-108 与
#       src/dsp/lossless.c:491。注意 **不能**用 WEBP_REDUCE_SIZE 省体积：
#       那会关掉缩放（src/dec/buffer_dec.c:198-208），而相册靠它做降采样。
#   NDEBUG
#       畸形 WebP 走返回码而不是 assert abort。
#   不定义 HAVE_CONFIG_H / WEBP_USE_THREAD / 任何 SIMD 宏。
#
# 源文件列表是**显式**的（不要改成 GLOB）：
#   漏 palette.c / random_utils.c / thread_utils.c 会在链接期报未定义符号，
#   因为它们只被内部路径引用，不做 GLOB 时最容易被漏掉。

set(WEBP_DEC_SRCS
    src/dec/alpha_dec.c
    src/dec/buffer_dec.c
    src/dec/frame_dec.c
    src/dec/idec_dec.c
    src/dec/io_dec.c
    src/dec/quant_dec.c
    src/dec/tree_dec.c
    src/dec/vp8_dec.c
    src/dec/vp8l_dec.c
    src/dec/webp_dec.c

    src/dsp/alpha_processing.c
    src/dsp/cpu.c
    src/dsp/dec.c
    src/dsp/dec_clip_tables.c
    src/dsp/filters.c
    src/dsp/lossless.c
    src/dsp/rescaler.c
    src/dsp/upsampling.c
    src/dsp/yuv.c

    src/utils/bit_reader_utils.c
    src/utils/color_cache_utils.c
    src/utils/filters_utils.c
    src/utils/huffman_utils.c
    src/utils/palette.c
    src/utils/quant_levels_dec_utils.c
    src/utils/rescaler_utils.c
    src/utils/random_utils.c
    src/utils/thread_utils.c
    src/utils/utils.c
)

idf_component_register(
    SRCS ${WEBP_DEC_SRCS}
    # 对外：让使用方写 #include "webp/decode.h"
    INCLUDE_DIRS src
    # 对内：libwebp 自己的源文件用 "src/dec/..." 这类带根前缀的 include
    PRIV_INCLUDE_DIRS .
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WEBP_SWAP_16BIT_CSP=1
    NDEBUG
)
