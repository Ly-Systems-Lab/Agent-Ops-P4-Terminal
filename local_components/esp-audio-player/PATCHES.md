# 本地补丁说明（ESP32P4_Pad 接管版）

这份代码是 **`chmorgan/esp-audio-player` v1.1.0** 的一份副本，由本项目**接管**。

接管理由：上游有一个会让播放**永久卡死**的缺陷，而修它必须改源码 —— 但
`managed_components/` 目录被 `.gitignore` 挡在版本控制之外，直接改那里**改动会丢**。
所以走组件管理器官方的 `override_path` 机制：见
`components/bsp_extra/idf_component.yml` 里 `chmorgan/esp-audio-player` 那一项。

**升级这份副本时**：重新从注册表拉一份上游源码覆盖本目录，然后按下面的清单重新打补丁。

---

## 本地补丁 #1 —— `audio_mp3.cpp` 的 `decode_mp3()`

**症状**：某些 MP3 播放时**全程无声**，且串口被 `mp3: status error -6` 刷屏
（实测约每秒 6000 条，52 秒 23 万条）。换别的歌正常。

**根因**：两个事实叠加。

1. 上游这里在解码失败时只返回 `DECODE_STATUS_NO_DATA_CONTINUE`，靠一句注释假设
   "下一次调用会从误判的帧头**之后**开始搜索"：

   ```c
   // The invalid frame data is skipped over as a search for the next frame
   // on the subsequent call to this function will start searching
   // AFTER the misdetected frmame header, dropping the invalid data.
   ```

2. 但被调用的 `MP3Decode()` 在帧头解析失败时**根本不推进 `*inbuf`**：

   ```c
   // libhelix-mp3/mp3dec.c
   fhBytes = UnpackFrameHeader(mp3DecInfo, *inbuf);
   if (fhBytes < 0)
       return ERR_MP3_INVALID_FRAMEHEADER;   /* 直接返回，*inbuf 原地不动 */
   *inbuf += fhBytes;                        /* 只有成功才前进 */
   ```

⇒ 下一轮 `MP3FindSyncWord()` 在同一位置又找到同一个假同步，**指针一步不动，死循环**。

**触发条件（实测复现）**：MP3 的 ID3v2 标签里含 UTF-16 文本，其 BOM 是 `FF FE` ——
这恰好命中 `MP3FindSyncWord()` 很松的判据（`0xFF` + 次字节高 3 位全 1）。
是否真的卡死，取决于 BOM 后面那两个字节能否凑出一个**合法**帧头：

| 文件 | 该处字节 | 采样率索引 | 结果 |
|---|---|---|---|
| 触发卡死的文件 | `FF FE EE 4F` | 3 = 保留值（非法） | 解析失败 → **死循环** |
| 侥幸正常的文件 | `FF FE 57 00` | 1（合法） | 解析通过 → 指针前进 → 越过假同步 |

**修法**：在失败分支里保证每轮至少前进 1 字节，并加边界判断，
防止 `read_ptr` 越过缓冲区末尾导致上层的 `unread_bytes`（`size_t`）下溢成巨大值。

```c
if (pInstance->read_ptr < pInstance->data_buf + pInstance->bytes_in_data_buf) {
    pInstance->read_ptr += 1;
}
```

**验证方式（可复现）**：拿一个 4:47 的 128 kbps MP3（ID3v2 标签含上述字节组合），
打补丁前：无声 + 刷屏；打补丁后：正常出声。
本项目还留了一个只剥掉 ID3 标签的同源文件作对照。

---

## 本地补丁 #2 —— 定位（seek）与真实播放位置

**起因**：上游**根本没有 seek**，也没有"当前播到哪了"的查询接口
（公开 API 只有 play / pause / resume / stop）。后果是任何基于它的进度条
都只能是假的 —— 界面上的时间只能靠"每秒加一"的本地计数器，
而本地计数器**和音频没有任何关系**：暂停、卡死时它照走不误。

**新增的公开接口**（`include/audio_player.h`）：

```c
esp_err_t audio_player_seek(long byte_offset, uint32_t position_ms);
uint32_t  audio_player_get_position_ms(void);
```

**一、定位为什么不能只写一句 `fseek`**

解码侧的状态必须一起清，否则等于没跳：

1. `fseek(fp, offset, SEEK_SET)`
2. **丢弃读缓冲** —— `mp3_data` 的 `bytes_in_data_buf / read_ptr / eof_reached`。
   不清的话下一轮会用跳转**前**残留在 `data_buf` 里的数据接着解。
3. **重建 MP3 解码器**（`MP3FreeDecoder` + `MP3InitDecoder`）。
   ★ 这条最容易漏：Helix 的**比特储备（bit reservoir）是跨帧的**，
   从任意位置起解时旧状态是垃圾，轻则失真重则报错。
4. **重新对齐帧边界** —— 不用自己做：`decode_mp3()` 开头的 `MP3FindSyncWord()`
   本来就会往后找到下一个合法帧头。最多丢一帧（约 26 ms）。

WAV 是无状态的顺序读，第 2/3 步不适用。

**二、位置为什么用"数已解码帧"而不是看时钟或 `ftell`**

在 `aplay_file()` 里，**每成功解出一帧**就把该帧的音频时长累加进 `played_us`：

```c
us = (uint64_t)frame_count * 1000000ULL / sample_rate;
```

- 放在 `aplay_file()` 里一处就同时覆盖 MP3 和 WAV（两者的 `output.fmt`
  在这一刻都已填好），不必去动两个解码器
- 用**微秒**而不是毫秒：每帧 26.12 ms，按毫秒累加会在 287 秒的歌上
  累计少 **1.3 秒**（10988 帧 × 0.12 ms）；用微秒只剩 4 ms
- 不用 `ftell(fp)`：播放器会**预读** ≈5820 字节，位置会系统性超前约 0.36 秒

**三、定位是异步的 —— 这是"修完还弹回去"的坑**

`audio_player_seek()` 只是把事件**排进队列**，音频任务要等解完当前帧
（约 26 ms）才会取走它。而 UI 每秒就来读一次位置 —— 在那之前读到的是
**跳转前**的位置 ⇒ 滑块照样弹回去，白修。
所以 `audio_player_seek()` 在排队成功后**立刻**把 `played_us` 拨到目标值。
`audio_instance_play()` 同理（否则切歌瞬间进度条会先窜到上一首的进度）。
读写在临界区里保护（写的有音频任务和调用方两个任务，读的是 UI 任务）。

**验证方式（用户实测）**：MusicPlayer 里拖动进度条 → 音乐从目标位置接着放，
松手后不再弹回。开发阶段的实测里有精度数据与已知误差上界。
