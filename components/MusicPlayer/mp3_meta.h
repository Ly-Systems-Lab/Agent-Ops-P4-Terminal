/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读 MP3 帧头算时长（秒）。算不出来返回 0。
 *
 * 做法：跳过 ID3v2 标签 → 在开头一小段里找第一个**连续两帧都合法**的 MPEG Layer III 帧头
 *       → 若第一帧是 Xing/Info（VBR 头）就用它记录的总帧数；
 *         否则按 CBR 估：(文件大小 − 音频起点) × 8 ÷ 比特率。
 * 只读文件开头几 KB，**不扫全文**。
 *
 * 为什么需要它：原厂把 LVGL demo 的占位常量（恒等于 60 秒）直接当成了所有曲目的时长 ——
 * 那个函数从头到尾没打开过任何文件，所以列表里每首歌都显示 1:00，与真实长度无关。 */
uint32_t mp3_file_duration_secs(const char *path);

/* ---- 定位（seek）用：把"第几秒"换算成"第几个字节" ----

   为什么是**按字节线性插值**，而不是解 Xing 的 TOC 表？
   实测过了：在 6 个文件、每个 99 个采样点上，
   线性插值最大误差 1.53s，TOC 最大误差 1.29s —— 同一量级，而线性插值在
   长文件上反而好得多（287 秒的 CBR 文件：0.02s vs 1.29s，因为 TOC 只有
   100 格，一格就是 2.9 秒）。

   直觉上"VBR 必须用 TOC"在这里不成立，原因是：TOC 治的是**时长估不准**，
   而我们的时长本来就来自 Xing 的总帧数（见 mp3_file_duration_secs）。
   时长一旦是对的，"累计字节 vs 时间"这条曲线本来就接近直线 ——
   VBR 的高频比特率波动在整条音轨上互相抵消了。 */
typedef struct {
    long     audio_start;   /* 第一个合法帧的字节偏移（ID3 之后）      */
    long     audio_end;     /* 音频区间右端（= 文件大小）              */
    uint32_t duration_s;    /* 真实时长（秒）；0 表示解析失败          */
} mp3_geom_t;

/* 解析上面那三个量。成功返回 1；0 表示这不是我们能定位的文件（非 MP3 / 解析失败）。 */
int mp3_file_geometry(const char *path, mp3_geom_t *out);

/* 把秒数换算成字节偏移（夹在 [audio_start, audio_end-1]）。
 * 解析失败返回 -1 —— 调用方**必须**判这个返回值：宁可不定位，也别拿 0 当合法偏移，
 * 那会从文件头开始放，听起来像"跳回开头了"。 */
long mp3_file_offset_for_secs(const char *path, uint32_t secs);

#ifdef __cplusplus
}
#endif
