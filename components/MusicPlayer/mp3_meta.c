/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mp3_meta.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* 最多读这么多字节来定位第一个帧头。ID3 已先跳过，几 KB 足够。 */
#define SCAN_BYTES   4096
/* ID3v2 标签声明长度超过这个值就认为不可信，当作没有标签处理。 */
#define ID3_MAX_SKIP (256 * 1024)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

typedef struct {
    int kbps;
    int hz;
    int samples_per_frame;
    int frame_len;
    int channels;       /* 1 = 单声道，2 = 立体声 */
    int is_mpeg1;
} mp3_frame_t;

/* 解析一个 MPEG Layer III 帧头，合法返回 1。
 *
 * 判据必须比 helix 的 MP3FindSyncWord() 严得多 —— 那个只认"0xFF + 次字节高 3 位全 1"，
 *    连 ID3 标签里 UTF-16 的 BOM (FF FE) 都会误判（后果见
 *    local_components/esp-audio-player/PATCHES.md 的「本地补丁 #1」）。
 *    这里直接按帧头各字段逐项校验，保留值一律判非法。 */
static int parse_layer3(const uint8_t *h, mp3_frame_t *out)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
        return 0;
    }

    const int ver   = (h[1] >> 3) & 0x3;    /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5, 1=保留 */
    const int layer = (h[1] >> 1) & 0x3;    /* 1 = Layer III */
    const int brx   = (h[2] >> 4) & 0xF;
    const int srx   = (h[2] >> 2) & 0x3;
    const int mode  = (h[3] >> 6) & 0x3;    /* 3 = 单声道 */

    if (ver == 1 || layer != 1 || brx == 0 || brx == 0xF || srx == 3) {
        return 0;
    }

    static const int hz_tab[4][3] = {
        {11025, 12000,  8000},              /* 0: MPEG2.5 */
        {    0,     0,     0},              /* 1: 保留，恒非法 */
        {22050, 24000, 16000},              /* 2: MPEG2 */
        {44100, 48000, 32000},              /* 3: MPEG1 */
    };
    static const int br1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int br2[16] = {0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160, 0};

    out->is_mpeg1          = (ver == 3);
    out->hz                = hz_tab[ver][srx];
    out->kbps              = out->is_mpeg1 ? br1[brx] : br2[brx];
    out->samples_per_frame = out->is_mpeg1 ? 1152 : 576;
    out->channels          = (mode == 3) ? 1 : 2;
    out->frame_len         = out->is_mpeg1
                             ? (144 * out->kbps * 1000) / out->hz
                             : ( 72 * out->kbps * 1000) / out->hz;

    return (out->hz > 0 && out->kbps > 0 && out->frame_len > 4) ? 1 : 0;
}

int mp3_file_geometry(const char *path, mp3_geom_t *out)
{
    if (path == NULL || out == NULL) {
        return 0;
    }
    out->audio_start = 0;
    out->audio_end   = 0;
    out->duration_s  = 0;

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return 0;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    uint8_t buf[SCAN_BYTES];
    long    audio_off = 0;

    /* 1) 跳过 ID3v2：10 字节头 + syncsafe 长度（每字节只用低 7 位） */
    if (fread(buf, 1, 10, fp) == 10 && memcmp(buf, "ID3", 3) == 0) {
        const long sz = (long)(((uint32_t)(buf[6] & 0x7F) << 21) |
                               ((uint32_t)(buf[7] & 0x7F) << 14) |
                               ((uint32_t)(buf[8] & 0x7F) << 7)  |
                                (uint32_t)(buf[9] & 0x7F));
        if (sz > 0 && sz <= ID3_MAX_SKIP) {
            audio_off = 10 + sz;
        }
    }
    if (fseek(fp, audio_off, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    /* 2) 找第一个合法帧头；并且要求"下一帧正好落在它该在的位置"——
     *    只靠单帧判断，假同步凑出来的概率太高。 */
    const size_t got = fread(buf, 1, sizeof(buf), fp);
    long         rel = -1;
    mp3_frame_t  fr;

    for (size_t i = 0; i + 4 <= got; ++i) {
        if (!parse_layer3(buf + i, &fr)) {
            continue;
        }
        const size_t next = i + (size_t)fr.frame_len;
        if (next + 4 <= got) {
            mp3_frame_t fr2;
            if (!parse_layer3(buf + next, &fr2)) {
                continue;                   /* 接不上 ⇒ 假同步，继续往后找 */
            }
        }
        rel = (long)i;
        break;
    }
    if (rel < 0) {
        fclose(fp);
        return 0;
    }

    uint32_t secs = 0;

    /* 3) 第一帧若是 Xing/Info（VBR 头），里面有总帧数，比按比特率估更准 */
    {
        const size_t side = fr.is_mpeg1 ? (fr.channels == 1 ? 17 : 32)
                                        : (fr.channels == 1 ?  9 : 17);
        const size_t xo = (size_t)rel + 4 + side;
        if (xo + 12 <= got &&
            (memcmp(buf + xo, "Xing", 4) == 0 || memcmp(buf + xo, "Info", 4) == 0)) {
            const uint32_t flags = be32(buf + xo + 4);
            if (flags & 0x1u) {             /* 第 0 位 = 有 frames 字段 */
                const uint32_t frames = be32(buf + xo + 8);
                secs = (uint32_t)(((uint64_t)frames * (uint64_t)fr.samples_per_frame) / (uint64_t)fr.hz);
            }
        }
    }

    /* 4) 没有 VBR 头 ⇒ 按 CBR 估 */
    if (secs == 0 && (long)st.st_size > audio_off + rel) {
        const uint64_t bits = (uint64_t)((long)st.st_size - audio_off - rel) * 8ull;
        secs = (uint32_t)(bits / ((uint64_t)fr.kbps * 1000ull));
    }

    fclose(fp);

    out->audio_start = audio_off + rel;
    out->audio_end   = (long)st.st_size;
    out->duration_s  = secs;

    return (secs > 0) ? 1 : 0;
}

uint32_t mp3_file_duration_secs(const char *path)
{
    mp3_geom_t g;
    return mp3_file_geometry(path, &g) ? g.duration_s : 0;
}

long mp3_file_offset_for_secs(const char *path, uint32_t secs)
{
    mp3_geom_t g;
    if (!mp3_file_geometry(path, &g)) {
        return -1;
    }

    const long span = g.audio_end - g.audio_start;
    if (span <= 0) {
        return -1;
    }
    if (secs >= g.duration_s) {
        return g.audio_end - 1;
    }

    /* 64 位中间量：span 可以到几十 MB，secs 可以到几千 —— 32 位乘会溢出 */
    long off = g.audio_start +
               (long)(((uint64_t)span * (uint64_t)secs) / (uint64_t)g.duration_s);

    if (off < g.audio_start) {
        off = g.audio_start;
    }
    if (off >= g.audio_end) {
        off = g.audio_end - 1;
    }
    return off;
}
