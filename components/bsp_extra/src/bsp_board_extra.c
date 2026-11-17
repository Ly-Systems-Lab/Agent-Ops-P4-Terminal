/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"          /* 探针用 */
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_vfs_fat.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/semphr.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

static const char *TAG = "bsp_extra_board";

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;
static i2s_chan_handle_t i2s_tx_chan;
static i2s_chan_handle_t i2s_rx_chan;
static const audio_codec_data_if_t *i2s_data_if;
static const audio_codec_gpio_if_t *speaker_gpio_if;
static const audio_codec_ctrl_if_t *speaker_ctrl_if;
static const audio_codec_if_t *speaker_codec_if;
static const audio_codec_ctrl_if_t *microphone_ctrl_if;
static const audio_codec_if_t *microphone_codec_if;

static bool _is_audio_init = false;
static bool _is_player_init = false;

static int _vloume_intensity = CODEC_DEFAULT_VOLUME;
/*
 * ---- 音频总线门闸 ----
 *
 * 这块 I2S 端口是**全双工**的：RX（ES7210 麦克风，TDM）与 TX（ES8311 喇叭，STD）各自
 * 有独立的 DMA，**读写本来就可以同时进行**。但设备的重配置（set_fs / stop / init /
 * 音量 / 静音）必须在"没有任何传输在跑"时做，否则会撕裂配置、甚至踩到已关闭的句柄。
 *
 * 原来只有**一把**递归锁，把整段阻塞传输都包在里面。Xiaozhi 同时用录音和放音，
 *    而录音任务（prio 8）每 10 ms 抢一次锁、抱着锁阻塞读 10 ms，等于把总线占掉近
 *    100%；放音任务（prio 6）只能在微秒级缝隙里抢 ⇒ **永远抢不到** ⇒ TTS 播不出来、
 *    状态机卡在 "Speaking"、连接被服务器反复断开（官网显示离线）。
 *    这是开发阶段实测确认过的。
 *
 * 现在拆成两把：读只和重配置互斥，写只和重配置互斥（写与写之间仍互斥，避免两个
 * 播放源交错出噪声）。读与读、读与写**不再互相阻塞**。
 *
 * 重配置路径保持**可重入**（同一个任务里 codec_init → set_fs → output_stop 会嵌套加锁），
 * 用 owner + depth 记账。
 */
static SemaphoreHandle_t audio_gate_rx;   /* 录音通路（与重配置互斥） */
static SemaphoreHandle_t audio_gate_tx;   /* 放音通路（与重配置、与其它写者互斥） */
static TaskHandle_t      audio_gate_owner; /* 重配置的持有者任务（可重入） */
static int               audio_gate_depth; /* 重配置重入深度 */
/* 有重配置在等/在跑时置位 —— 录音看到就让路（丢一拍 10 ms 无害），
 * 这样 100% 占空比的录音不会把重配置饿死。 */
static volatile bool     audio_reconfig_pending;

static audio_player_cb_t audio_idle_callback = NULL;
static void *audio_idle_cb_user_data = NULL;
static char audio_file_path[128];

static esp_err_t audio_lock(void);
static void audio_unlock(void);

#define BSP_EXTRA_ES7210_CODEC_ADDR      ES7210_CODEC_DEFAULT_ADDR

#define BSP_EXTRA_I2S_GPIO_CFG  \
    {                           \
        .mclk = BSP_I2S_MCLK,   \
        .bclk = BSP_I2S_SCLK,   \
        .ws = BSP_I2S_LCLK,     \
        .dout = BSP_I2S_DOUT,   \
        .din = BSP_I2S_DSIN,    \
        .invert_flags = {       \
            .mclk_inv = false,  \
            .bclk_inv = false,  \
            .ws_inv = false,    \
        },                      \
    }

/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    // Volume saved when muting and restored when unmuting. Restoring volume is necessary
    // as es8311_set_voice_mute(true) results in voice volume (REG32) being set to zero.

    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock player mute failed");
    esp_err_t ret = bsp_extra_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);

    // restore the voice volume upon unmuting
    if (ret == ESP_OK && setting == AUDIO_PLAYER_UNMUTE) {
        ret = esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity);
    }
    audio_unlock();

    return ret;
}

static void audio_callback(audio_player_cb_ctx_t *ctx)
{
    if (audio_idle_callback) {
        ctx->user_ctx = audio_idle_cb_user_data;
        audio_idle_callback(ctx);
    }
}

bool bsp_extra_display_lock(int32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void bsp_extra_display_unlock(void)
{
    esp_lv_adapter_unlock();
}

static esp_err_t audio_gate_init(void)
{
    if (!audio_gate_rx) {
        audio_gate_rx = xSemaphoreCreateMutex();
    }
    if (!audio_gate_tx) {
        audio_gate_tx = xSemaphoreCreateMutex();
    }
    return (audio_gate_rx && audio_gate_tx) ? ESP_OK : ESP_ERR_NO_MEM;
}

/*
 * 数据通路（录音 / 放音）：只跟"重配置"互斥，读写彼此不互斥。
 *
 * 有重配置在等或正在跑时（audio_reconfig_pending），数据通路一律**先让路**：
 * 重配置要的是"总线上一笔传输都没有"，而录音是 100% 占空比的连续流，
 * 不让路的话重配置会被它饿死（原来那把锁就是这个病，只是受害者反过来了）。
 * 让路的代价只是重配置那几毫秒里少读/少写一拍，重配置本身很短。
 *
 * 全程用调用方给的 timeout_ms 兜底，**不会无限等** —— 这是原来最大的问题：
 * 参数收下了却被 `(void)timeout_ms;` 丢掉，于是任何争用都退化成永久挂起。
 */
static esp_err_t audio_data_lock(SemaphoreHandle_t gate, uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(audio_gate_init(), TAG, "Init audio gate failed");
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    if (timeout == 0) {
        timeout = 1;                    /* 0 会立刻判超时，兜个底 */
    }
    const TickType_t start = xTaskGetTickCount();

    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        if (audio_reconfig_pending) {
            vTaskDelay(1);
            continue;
        }
        if (xSemaphoreTake(gate, timeout - elapsed) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        if (!audio_reconfig_pending) {
            return ESP_OK;
        }
        /* 刚拿到就被重配置插队了 ⇒ 让出去，重来 */
        xSemaphoreGive(gate);
    }
}

static void audio_data_unlock(SemaphoreHandle_t gate)
{
    if (gate) {
        xSemaphoreGive(gate);
    }
}

/*
 * 重配置通路（独占）：把两条数据通路都关掉再动手。
 * **可重入**：同一个任务里 codec_init → set_fs → output_stop 会嵌套加锁，
 * 用 owner + depth 记账（原来靠递归锁，别把这个性质弄丢）。
 */
static esp_err_t audio_lock(void)
{
    ESP_RETURN_ON_ERROR(audio_gate_init(), TAG, "Init audio gate failed");
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (audio_gate_owner == self) {
        audio_gate_depth++;
        return ESP_OK;
    }

    audio_reconfig_pending = true;      /* 先挡住数据通路，再收闸 */

    if (xSemaphoreTake(audio_gate_tx, portMAX_DELAY) != pdTRUE) {
        audio_reconfig_pending = false;
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(audio_gate_rx, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(audio_gate_tx);
        audio_reconfig_pending = false;
        return ESP_ERR_TIMEOUT;
    }

    audio_gate_owner = self;
    audio_gate_depth = 1;
    return ESP_OK;
}

static void audio_unlock(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (audio_gate_owner != self) {
        return;
    }
    if (audio_gate_depth > 1) {
        audio_gate_depth--;
        return;
    }
    audio_gate_owner = NULL;
    audio_gate_depth = 0;
    xSemaphoreGive(audio_gate_rx);
    xSemaphoreGive(audio_gate_tx);
    audio_reconfig_pending = false;     /* 放行数据通路 */
}

static void update_codec_result(esp_err_t *result, esp_err_t codec_result)
{
    if (*result == ESP_OK && codec_result != ESP_OK) {
        *result = codec_result;
    }
}

static void update_codec_interface_result(esp_err_t *result, int codec_result)
{
    if (*result == ESP_OK && codec_result != ESP_CODEC_DEV_OK) {
        *result = ESP_FAIL;
    }
}

static esp_err_t delete_i2s_channel(i2s_chan_handle_t *channel)
{
    if (!channel || !*channel) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    esp_err_t err = i2s_channel_disable(*channel);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ret = err;
    }
    update_codec_result(&ret, i2s_del_channel(*channel));
    *channel = NULL;
    return ret;
}

static esp_err_t delete_speaker_codec(void)
{
    esp_err_t ret = ESP_OK;

    if (play_dev_handle) {
        update_codec_result(&ret, esp_codec_dev_close(play_dev_handle));
        esp_codec_dev_delete(play_dev_handle);
        play_dev_handle = NULL;
    }
    if (speaker_codec_if) {
        update_codec_interface_result(&ret, audio_codec_delete_codec_if(speaker_codec_if));
        speaker_codec_if = NULL;
    }
    if (speaker_ctrl_if) {
        update_codec_interface_result(&ret, audio_codec_delete_ctrl_if(speaker_ctrl_if));
        speaker_ctrl_if = NULL;
    }
    if (speaker_gpio_if) {
        update_codec_interface_result(&ret, audio_codec_delete_gpio_if(speaker_gpio_if));
        speaker_gpio_if = NULL;
    }
    return ret;
}

static esp_err_t delete_microphone_codec(void)
{
    esp_err_t ret = ESP_OK;

    if (record_dev_handle) {
        update_codec_result(&ret, esp_codec_dev_close(record_dev_handle));
        esp_codec_dev_delete(record_dev_handle);
        record_dev_handle = NULL;
    }
    if (microphone_codec_if) {
        update_codec_interface_result(&ret, audio_codec_delete_codec_if(microphone_codec_if));
        microphone_codec_if = NULL;
    }
    if (microphone_ctrl_if) {
        update_codec_interface_result(&ret, audio_codec_delete_ctrl_if(microphone_ctrl_if));
        microphone_ctrl_if = NULL;
    }
    return ret;
}

static esp_err_t bsp_extra_audio_deinit(void)
{
    esp_err_t ret = ESP_OK;

    update_codec_result(&ret, delete_microphone_codec());
    update_codec_result(&ret, delete_speaker_codec());
    if (i2s_data_if) {
        update_codec_interface_result(&ret, audio_codec_delete_data_if(i2s_data_if));
        i2s_data_if = NULL;
    }
    update_codec_result(&ret, delete_i2s_channel(&i2s_rx_chan));
    update_codec_result(&ret, delete_i2s_channel(&i2s_tx_chan));
    _is_audio_init = false;
    return ret;
}

static esp_err_t bsp_extra_audio_bus_init(void)
{
    ESP_RETURN_ON_FALSE(
        !i2s_tx_chan && !i2s_rx_chan && !i2s_data_if,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Audio bus is already initialized"
    );

    esp_err_t ret = ESP_OK;
    /* 喂送的 DMA 深度。
     *
     * 历史：从 240 改到 480（**6 × 480 × 4 = 11,520 B ≈ 65.3 ms**）。
     * 2026-10-20 实测（`[APROBE] idle_max`）：**双击旋转那一秒**的喂送空转
     * 能到 **72.5 ms**（gq02），超过 65.3 ms ⇒ 管子见底 ⇒ `auto_clear` 补零
     * ⇒ 用户听到"偶尔的电流杂音"。⇒ **加深到 720 帧 = 98 ms**（约 25 ms 余量）。
     *
     * **绝对不要把 TX / RX 拆成两次 `i2s_new_channel()` 去单独设深度。**
     * 试过（2026-10-20）：TX 720 / RX 480 分开建 ⇒ 两条通道不再共享同一套
     * 时钟，RX 的 TDM 配置（4 slot + bclk_div=8）把共用的 BCLK/WS 改掉
     * ⇒ **放音直接变成密集的雪花噪声**（画面正常）。必须**一次建两条**。
     *
     * 代价两条：① 音画延迟 65 → 98 ms（本项目的视频不是逐字对白，实测可接受；
     *    将来做口播类内容要回头重估）；② 内部 DMA RAM 多占约 **17 KB**
     *    （TX +5.8 KB，RX 的 TDM 4-slot 也一起加深 +11.5 KB）——
     *    内部 RAM 本来就被 TLS + 音频管线挤得很紧（"还剩几 KB"不等于能分配），如果 AIChats 出现
     *    `sdio_rx_get_buffer` 之类分配失败，优先回来把这个值调回 480。 */
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    channel_config.dma_frame_num = 720;     /* 6 × 720 × 4 B = 17,280 B ≈ 98 ms */

    i2s_std_config_t tx_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = BSP_EXTRA_I2S_GPIO_CFG,
    };
    i2s_tdm_config_t rx_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(CODEC_VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3
        ),
        .gpio_cfg = BSP_EXTRA_I2S_GPIO_CFG,
    };
    tx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tx_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    rx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx_config.clk_cfg.bclk_div = 8;
    rx_config.slot_cfg.total_slot = CODEC_VOICE_INPUT_CHANNELS;
    rx_config.gpio_cfg.dout = I2S_GPIO_UNUSED;

    ESP_GOTO_ON_ERROR(
        i2s_new_channel(&channel_config, &i2s_tx_chan, &i2s_rx_chan),
        fail,
        TAG,
        "Create I2S channels failed"
    );
    ESP_GOTO_ON_ERROR(
        i2s_channel_init_std_mode(i2s_tx_chan, &tx_config),
        fail,
        TAG,
        "Initialize I2S TX failed"
    );
    ESP_GOTO_ON_ERROR(
        i2s_channel_init_tdm_mode(i2s_rx_chan, &rx_config),
        fail,
        TAG,
        "Initialize I2S TDM RX failed"
    );
    ESP_GOTO_ON_ERROR(
        i2s_channel_enable(i2s_tx_chan), fail, TAG, "Enable I2S TX failed"
    );
    ESP_GOTO_ON_ERROR(
        i2s_channel_enable(i2s_rx_chan), fail, TAG, "Enable I2S RX failed"
    );

    audio_codec_i2s_cfg_t i2s_config = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_config);
    ESP_GOTO_ON_FALSE(
        i2s_data_if, ESP_ERR_NO_MEM, fail, TAG, "Create codec I2S data interface failed"
    );
    return ESP_OK;

fail:
    {
        esp_err_t cleanup_ret = bsp_extra_audio_deinit();
        if (cleanup_ret != ESP_OK) {
            ESP_LOGE(TAG, "Clean up audio bus failed: %s", esp_err_to_name(cleanup_ret));
        }
    }
    return ret;
}

static esp_codec_dev_handle_t create_speaker_codec(void)
{
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialize codec I2C bus failed: %s", esp_err_to_name(ret));
        return NULL;
    }
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(i2c_bus, NULL, TAG, "Codec I2C bus is unavailable");

    speaker_gpio_if = audio_codec_new_gpio();
    if (!speaker_gpio_if) {
        goto fail;
    }

    audio_codec_i2c_cfg_t i2c_config = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    speaker_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!speaker_ctrl_if) {
        goto fail;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = speaker_ctrl_if,
        .gpio_if = speaker_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    speaker_codec_if = es8311_codec_new(&codec_config);
    if (!speaker_codec_if) {
        goto fail;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker_codec_if,
        .data_if = i2s_data_if,
    };
    play_dev_handle = esp_codec_dev_new(&device_config);
    if (!play_dev_handle) {
        goto fail;
    }
    return play_dev_handle;

fail:
    (void)delete_speaker_codec();
    return NULL;
}

static esp_codec_dev_handle_t create_microphone_codec(void)
{
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialize codec I2C bus failed: %s", esp_err_to_name(ret));
        return NULL;
    }
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(i2c_bus, NULL, TAG, "Codec I2C bus is unavailable");

    audio_codec_i2c_cfg_t i2c_config = {
        .port = BSP_I2C_NUM,
        .addr = BSP_EXTRA_ES7210_CODEC_ADDR,
        .bus_handle = i2c_bus,
    };
    microphone_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!microphone_ctrl_if) {
        goto fail;
    }

    es7210_codec_cfg_t codec_config = {
        .ctrl_if = microphone_ctrl_if,
        .mic_selected = BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK,
    };
    microphone_codec_if = es7210_codec_new(&codec_config);
    if (!microphone_codec_if) {
        goto fail;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone_codec_if,
        .data_if = i2s_data_if,
    };
    record_dev_handle = esp_codec_dev_new(&device_config);
    if (!record_dev_handle) {
        goto fail;
    }
    return record_dev_handle;

fail:
    (void)delete_microphone_codec();
    return NULL;
}

esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (bytes_read) {
        *bytes_read = 0;
    }
    ESP_RETURN_ON_FALSE(record_dev_handle && audio_buffer, ESP_ERR_INVALID_STATE, TAG, "Recorder is not initialized");

    /* 录音走"读"闸：只与重配置互斥，**不再和放音互相抢总线**（关键改动） */
    ESP_RETURN_ON_ERROR(audio_data_lock(audio_gate_rx, timeout_ms), TAG, "Lock audio input failed");
    esp_err_t ret = esp_codec_dev_read(record_dev_handle, audio_buffer, len);
    if (ret == ESP_OK && bytes_read) {
        *bytes_read = len;
    }
    audio_data_unlock(audio_gate_rx);
    return ret;
}

esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (bytes_written) {
        *bytes_written = 0;
    }
    ESP_RETURN_ON_FALSE(play_dev_handle && audio_buffer, ESP_ERR_INVALID_STATE, TAG, "Player is not initialized");

    /* 放音走"写"闸：与重配置、与其它写者互斥；**不再被录音饿死**（关键改动） */
    ESP_RETURN_ON_ERROR(audio_data_lock(audio_gate_tx, timeout_ms), TAG, "Lock audio output failed");
    esp_err_t ret = esp_codec_dev_write(play_dev_handle, audio_buffer, len);
    if (ret == ESP_OK && bytes_written) {
        *bytes_written = len;
    }
    audio_data_unlock(audio_gate_tx);
    return ret;
}

esp_err_t bsp_extra_codec_output_stop(void)
{
    if (!play_dev_handle) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock playback codec failed");
    esp_err_t ret = esp_codec_dev_close(play_dev_handle);
    audio_unlock();
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ret;
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    ESP_RETURN_ON_FALSE(play_dev_handle && record_dev_handle, ESP_ERR_INVALID_STATE, TAG, "Codec is not initialized");
    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock playback reconfiguration failed");
    // Ordinary media/output reconfiguration must not close the ES7210 voice
    // capture session. Voice mode uses bsp_extra_codec_set_voice_fs(), which
    // intentionally owns both sides of the shared bus.
    esp_err_t ret = bsp_extra_codec_output_stop();
    if (ret != ESP_OK) {
        audio_unlock();
        ESP_LOGE(TAG, "Close playback codec failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_codec_dev_sample_info_t output_fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    ret = esp_codec_dev_open(play_dev_handle, &output_fs);
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_out_mute(play_dev_handle, false);
    }
    if (ret != ESP_OK) {
        (void)esp_codec_dev_close(play_dev_handle);
    }
    audio_unlock();
    return ret;
}

esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate, uint32_t bits_cfg,
                                       uint8_t record_channels,
                                       uint16_t record_tdm_slot_mask,
                                       uint16_t record_mic_gain_mask)
{
    ESP_RETURN_ON_FALSE(play_dev_handle && record_dev_handle, ESP_ERR_INVALID_STATE, TAG, "Codec is not initialized");
    ESP_RETURN_ON_FALSE(record_channels > 0 && record_channels <= 16 &&
                        record_tdm_slot_mask != 0 &&
                        (record_tdm_slot_mask >> record_channels) == 0 &&
                        record_mic_gain_mask != 0 &&
                        (record_mic_gain_mask >> 4) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid record channel selection");
    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock voice codec reconfiguration failed");
    esp_err_t ret = bsp_extra_codec_dev_stop();
    if (ret != ESP_OK) {
        audio_unlock();
        ESP_LOGE(TAG, "Close codec devices failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_codec_dev_sample_info_t output_fs = {
        .sample_rate = rate,
        .channel = I2S_SLOT_MODE_STEREO,
        .bits_per_sample = bits_cfg,
    };
    esp_codec_dev_sample_info_t input_fs = {
        .sample_rate = rate,
        .channel = record_channels,
        .channel_mask = record_tdm_slot_mask,
        .bits_per_sample = bits_cfg,
    };

    ret = esp_codec_dev_open(play_dev_handle, &output_fs);
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_out_mute(play_dev_handle, false);
    }
    if (ret != ESP_OK) {
        (void)bsp_extra_codec_dev_stop();
        audio_unlock();
        return ret;
    }

    ret = esp_codec_dev_open(record_dev_handle, &input_fs);
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_in_channel_gain(
                  record_dev_handle, record_mic_gain_mask, CODEC_DEFAULT_ADC_VOLUME
              );
    }
    if (ret != ESP_OK) {
        (void)bsp_extra_codec_dev_stop();
    }
    audio_unlock();
    return ret;
}

esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set)
{
    (void)volume_set;
    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock codec volume failed");
    _vloume_intensity = volume;
    esp_err_t ret = esp_codec_dev_set_out_vol(play_dev_handle, volume);
    audio_unlock();
    ESP_RETURN_ON_ERROR(ret, TAG, "Set Codec volume failed");

    ESP_LOGI(TAG, "Setting volume: %d", volume);

    return ESP_OK;
}

int bsp_extra_codec_volume_get(void)
{
    return _vloume_intensity;
}

esp_err_t bsp_extra_codec_mute_set(bool enable)
{
    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock codec mute failed");
    esp_err_t ret = esp_codec_dev_set_out_mute(play_dev_handle, enable);
    audio_unlock();
    return ret;
}

esp_err_t bsp_extra_codec_dev_stop(void)
{
    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock codec stop failed");
    esp_err_t ret = ESP_OK;

    if (record_dev_handle) {
        update_codec_result(&ret, esp_codec_dev_close(record_dev_handle));
    }
    if (play_dev_handle) {
        update_codec_result(&ret, esp_codec_dev_close(play_dev_handle));
    }

    audio_unlock();
    return ret;
}

esp_err_t bsp_extra_codec_dev_resume(void)
{
    return bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

esp_err_t bsp_extra_codec_init(void)
{
    ESP_RETURN_ON_ERROR(audio_lock(), TAG, "Lock codec initialization failed");
    if (_is_audio_init) {
        audio_unlock();
        return ESP_OK;
    }

    esp_err_t ret = bsp_extra_audio_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialize mixed STD/TDM audio bus failed: %s", esp_err_to_name(ret));
        audio_unlock();
        return ret;
    }

    if (!create_speaker_codec() || !create_microphone_codec()) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = bsp_extra_codec_set_fs(
              CODEC_DEFAULT_SAMPLE_RATE,
              CODEC_DEFAULT_BIT_WIDTH,
              CODEC_DEFAULT_CHANNEL
          );
    if (ret != ESP_OK) {
        goto cleanup;
    }
    _is_audio_init = true;
    audio_unlock();
    return ESP_OK;

cleanup:
    {
        esp_err_t cleanup_ret = bsp_extra_audio_deinit();
        if (cleanup_ret != ESP_OK) {
            ESP_LOGE(TAG, "Clean up codec initialization failed: %s", esp_err_to_name(cleanup_ret));
        }
    }
    audio_unlock();
    return ret;
}

esp_err_t bsp_extra_player_init(void)
{
    if (_is_player_init) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_extra_codec_init(), TAG, "Initialize codec failed");
    ESP_RETURN_ON_ERROR(bsp_extra_codec_dev_resume(), TAG, "Resume output codec failed");
    audio_player_config_t config = { .mute_fn = audio_mute_function,
                                     .write_fn = bsp_extra_i2s_write,
                                     .clk_set_fn = bsp_extra_codec_set_fs,
                                     .priority = 5
                                   };
    ESP_RETURN_ON_ERROR(audio_player_new(config), TAG, "audio_player_init failed");
    audio_player_callback_register(audio_callback, NULL);

    _is_player_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_del(void)
{
    esp_err_t ret = audio_player_delete();
    ESP_RETURN_ON_ERROR(ret, TAG, "Delete audio player failed");
    _is_player_init = false;
    ESP_RETURN_ON_ERROR(bsp_extra_codec_output_stop(), TAG, "Stop playback codec failed");

    return ESP_OK;
}

esp_err_t bsp_extra_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    file_iterator_instance_t *file_iterator = file_iterator_new(path);
    ESP_RETURN_ON_FALSE(file_iterator, ESP_FAIL, TAG, "file_iterator_new failed, %s", path);

    *ret_instance = file_iterator;

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index)
{
    ESP_RETURN_ON_FALSE(instance, ESP_FAIL, TAG, "instance is NULL");

    ESP_LOGI(TAG, "play_index(%d)", index);
    char filename[128];
    int retval = file_iterator_get_full_path_from_index(instance, index, filename, sizeof(filename));
    ESP_RETURN_ON_FALSE(retval != 0, ESP_FAIL, TAG, "file_iterator_get_full_path_from_index failed");

    ESP_LOGI(TAG, "opening file '%s'", filename);
    FILE *fp = fopen(filename, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", filename);
    esp_err_t ret = audio_player_play(fp);
    if (ret != ESP_OK) {
        fclose(fp);
        ESP_LOGE(TAG, "audio_player_play failed: %s", esp_err_to_name(ret));
        return ret;
    }

    file_iterator_set_index(instance, index);
    memcpy(audio_file_path, filename, sizeof(audio_file_path));

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    ESP_LOGI(TAG, "opening file '%s'", file_path);
    FILE *fp = fopen(file_path, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", file_path);
    esp_err_t ret = audio_player_play(fp);
    if (ret != ESP_OK) {
        fclose(fp);
        ESP_LOGE(TAG, "audio_player_play failed: %s", esp_err_to_name(ret));
        return ret;
    }

    memcpy(audio_file_path, file_path, sizeof(audio_file_path));

    return ESP_OK;
}

void bsp_extra_player_register_callback(audio_player_cb_t cb, void *user_data)
{
    audio_idle_callback = cb;
    audio_idle_cb_user_data = user_data;
}

bool bsp_extra_player_is_playing_by_path(const char *file_path)
{
    return (strcmp(audio_file_path, file_path) == 0);
}

bool bsp_extra_player_is_playing_by_index(file_iterator_instance_t *instance, int index)
{
    return (index == file_iterator_get_index(instance));
}
