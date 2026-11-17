/**
 * @file lv_demo_music_main.h
 *
 */

#ifndef APP_DEMO_MUSIC_MAIN_H
#define APP_DEMO_MUSIC_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lv_demo_music.h"
#include "bsp_board_extra.h"
#if APP_DEMO_MUSIC_ENABLE

/*********************
 *      DEFINES
 *********************/

/* 列表里最多画多少个曲目按钮。
 * 原值是 5 —— 正好等于出厂那 5 首示例曲，于是"多出来的文件被静默丢掉"：
 *    扫描明明拿到 6 个，界面只画 5 个，而且**一个字都不报**。
 *    现在改成 32（16 MB 分区放满整首歌也到不了这个数），
 *    并且 lv_demo_music() 里加了超限告警 —— 让它以后不可能再静默发生。 */
#define ACTIVE_TRACK_CNT    32

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
lv_obj_t * lv_demo_music_main_create(lv_obj_t * parent, file_iterator_instance_t *iterator);
void lv_demo_music_main_close(void);
void lv_demo_music_play(uint32_t id);
void lv_demo_music_resume(void);
void lv_demo_music_pause(void);
void lv_demo_music_exit_pause(void);
void lv_demo_music_album_next(bool next);

/**********************
 *      MACROS
 **********************/
#endif /*LV_USE_DEMO_MUSIC*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*LV_DEMO_MUSIC_MAIN_H*/
