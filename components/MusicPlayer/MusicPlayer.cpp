/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "MusicPlayer.hpp"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:MusicPlayer"
#include "esp_lib_utils.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "gui_music/lv_demo_music.h"
#include "gui_music/lv_demo_music_main.h"

#define MUSIC_DIR   BSP_SPIFFS_MOUNT_POINT "/music"   /* 出厂示例曲（只读资源分区） */
/* 网页控制台上传文件的落点。**必须与 brookesia_web_console 的 WebConsole::UPLOAD_DIR 一致** ——
 * 这里刻意不去依赖那个组件：音乐播放器不该为了一个路径常量去依赖一个 web 服务。
 * （分区本身由 main.cpp 在开机时挂载；挂不上时这里只是扫不到东西。） */
#define UPLOAD_DIR  "/data/upload"
/* SD 卡根目录（BSP 的挂载点宏，不用自己写死 "/sdcard"）。分区由 main.cpp 在开机时挂好。
 * 只扫**根目录**、不递归：用户放素材的习惯就是直接丢在卡根（2026-10-14 确认过）。
 * 卡没插/没挂上时 opendir 会失败，这里只是扫不到东西，不是错误。 */
#define SD_DIR      BSP_SD_MOUNT_POINT

LV_IMG_DECLARE(img_app_musicplayer);

static const char *TAG = "MusicPlayer";

/* 只认播放器真能解的两种（CONFIG_AUDIO_PLAYER_ENABLE_MP3 / _WAV 都是 y）。 */
static bool is_audio_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    return strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0;
}

/* 收集 dir 下符合扩展名的普通文件，存**去掉开头斜杠的相对路径**（如 "data/upload/x.mp3"）。
 * 存相对路径是刻意的：file_iterator 拼完整路径用的是 snprintf("%s/%s", directory_path, name)，
 * 而我们要把两个目录合并进一个列表 ⇒ directory_path 置空串，拼出来正好是 "/data/upload/x.mp3"，
 * 播放函数一行都不用改。 */
static void collect_audio(const char *dir, std::vector<std::string> &out)
{
    DIR *dp = opendir(dir);
    if (dp == nullptr) {
        ESP_LOGW(TAG, "Cannot scan %s (partition not mounted?)", dir);
        return;
    }

    struct dirent *ent = nullptr;
    while ((ent = readdir(dp)) != nullptr) {
        /* ★ 这一行同时挡掉 "."、".." 和隐藏文件，而且**必须挡**：
         *   上游 esp-file-iterator 的 file_scan() 什么都不过滤（readdir 数到啥算啥），
         *   直接把目录指到 /data/upload 的话，上传的图片会被当成一首歌列进播放列表。 */
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!is_audio_file(ent->d_name)) {
            continue;
        }

        const std::string full = std::string(dir) + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        std::string rel = dir;
        if (!rel.empty() && rel[0] == '/') {
            rel.erase(0, 1);
        }
        out.push_back(rel + "/" + ent->d_name);
    }
    closedir(dp);
}

/* 自己构造一份「合并 + 过滤」过的迭代器。
 * 不用 bsp_extra_file_instance_init()：它只认一个目录，且完全不过滤扩展名。
 * 返回 nullptr 只表示**分配失败**；目录为空是正常的（count=0，界面自己会显示 No tracks found）。 */
static file_iterator_instance_t *build_track_iterator(void)
{
    std::vector<std::string> tracks;
    collect_audio(MUSIC_DIR, tracks);     /* 出厂示例曲在前 */
    collect_audio(UPLOAD_DIR, tracks);    /* 上传的追加在后 */
    collect_audio(SD_DIR, tracks);        /* SD 卡根目录的追加在最后 */

    auto *it = static_cast<file_iterator_instance_t *>(calloc(1, sizeof(file_iterator_instance_t)));
    if (it == nullptr) {
        ESP_LOGE(TAG, "calloc iterator failed");
        return nullptr;
    }

    it->count = tracks.size();
    it->index = 0;
    /* 空串而不是 NULL：snprintf("%s/%s", NULL, ...) 是未定义行为 */
    it->directory_path = strdup("");
    it->list = static_cast<char **>(calloc(tracks.size() + 1, sizeof(char *)));

    if (it->directory_path == nullptr || it->list == nullptr) {
        ESP_LOGE(TAG, "alloc track list failed");
        free(it->list);
        free(const_cast<char *>(it->directory_path));
        free(it);
        return nullptr;
    }

    for (size_t i = 0; i < tracks.size(); ++i) {
        it->list[i] = strdup(tracks[i].c_str());
        ESP_LOGI(TAG, "  [%u] %s", static_cast<unsigned>(i), tracks[i].c_str());
    }

    ESP_LOGI(TAG, "Total %u track(s) = %s + %s + %s",
             static_cast<unsigned>(it->count), MUSIC_DIR, UPLOAD_DIR, SD_DIR);
    return it;
}

static void destroy_file_iterator(file_iterator_instance_t **iterator)
{
    if (!iterator || !*iterator) {
        return;
    }
    file_iterator_instance_t *instance = *iterator;
    if (instance->list) {
        for (size_t i = 0; i < instance->count; ++i) {
            free(instance->list[i]);
        }
        free(instance->list);
    }
    free(const_cast<char *>(instance->directory_path));
    free(instance);
    *iterator = nullptr;
}

namespace esp_brookesia::apps
{

    MusicPlayer *MusicPlayer::_instance = nullptr;

    MusicPlayer *MusicPlayer::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new MusicPlayer(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    /* 让「MQTT 命令打开音乐」真的出声 —— 等价于用户点了列表第一行。
     * 只保证"请求已发出"：`bsp_extra_player_play_index()` 是异步的，
     *    立刻读 `audio_player_get_state()` 只会读到上一首的状态。真失败时
     *    `lv_demo_music_resume()` 自己会打 `Failed to start track`。 */
    bool MusicPlayer::playFirst(void)
    {
        if (!_ui_active) {
            ESP_LOGW(TAG, "playFirst: UI 还没建好（app 不在运行？）");
            return false;
        }
        const uint32_t count = lv_demo_music_get_track_count();
        if (count == 0) {
            ESP_LOGW(TAG, "playFirst: 曲目列表为空（/spiffs/music、/data/upload、SD 根都没有歌？）");
            return false;
        }
        lv_demo_music_play(0);
        ESP_LOGI(TAG, "playFirst: 已发起播放第 1 首（共 %u 首）", (unsigned)count);
        return true;
    }

    MusicPlayer::MusicPlayer(bool use_status_bar, bool use_navigation_bar) : App("MusicPlayer", &img_app_musicplayer, true, use_status_bar, use_navigation_bar),
                                                                             _file_iterator(NULL)
    {
    }

    MusicPlayer::~MusicPlayer()
    {
        close();
    }

    bool MusicPlayer::run(void)
    {
        ESP_UTILS_LOGD("Run");

        if (_player_active || _ui_active || _file_iterator) {
            (void)close();
        }

        esp_err_t ret = bsp_extra_player_init();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Audio player initialization failed: %s", esp_err_to_name(ret));
            return false;
        }
        _player_active = true;

        /* 自己扫 /spiffs/music + /data/upload，按扩展名过滤后再合并（见文件上方说明）。 */
        _file_iterator = build_track_iterator();
        if (_file_iterator == nullptr)
        {
            ESP_LOGE(TAG, "Build track list failed");
            (void)bsp_extra_player_del();
            _player_active = false;
            return false;
        }

        ESP_LOGI(
            TAG,
            "Loaded %u music track(s)",
            static_cast<unsigned>(file_iterator_get_count(_file_iterator))
        );
        lv_demo_music(lv_scr_act(), _file_iterator);
        _ui_active = true;
        return true;
    }

    bool MusicPlayer::back(void)
    {
        ESP_UTILS_LOGD("Back");
        // If the app needs to exit, call notifyCoreClosed() to notify the core to close the app
        ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
        return true;
    }

    bool MusicPlayer::close(void)
    {
        ESP_UTILS_LOGD("Close");
        bool ok = true;

        if (_ui_active) {
            lv_demo_music_close();
            _ui_active = false;
        }

        if (_player_active) {
            esp_err_t pause_ret = audio_player_pause();
            if (pause_ret != ESP_OK && pause_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Pause audio player failed: %s", esp_err_to_name(pause_ret));
                ok = false;
            }
            esp_err_t delete_ret = bsp_extra_player_del();
            if (delete_ret != ESP_OK) {
                ESP_LOGE(TAG, "Delete audio player failed: %s", esp_err_to_name(delete_ret));
                ok = false;
            } else {
                _player_active = false;
            }
        }

        /* Keep the iterator alive if the audio task could not be stopped; a
         * later close attempt can then finish without leaving stale state. */
        if (_file_iterator && !_player_active) {
            destroy_file_iterator(&_file_iterator);
        }
        return ok;
    }

    bool MusicPlayer::init()
    {
        ESP_UTILS_LOGD("Init");
        return true;
    }

    bool MusicPlayer::deinit()
    {
        ESP_UTILS_LOGD("Deinit");
        return close();
    }

    bool MusicPlayer::pause()
    {
        ESP_UTILS_LOGD("Pause UI; keep audio playback state");
        // Brookesia calls pause() when another screen becomes active. Audio is
        // intentionally left untouched so playback can continue in background.
        return true;
    }

    bool MusicPlayer::resume()
    {
        ESP_UTILS_LOGD("Resume UI; keep audio playback state");
        // The UI and audio player remain alive while the app is backgrounded.
        // Avoid restarting audio here, especially when the user paused it.
        return true;
    }

} // namespace esp_brookesia::apps
