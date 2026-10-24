// main/radio_player.h —— 直播流播放器（蜻蜓 FM 音乐台）。
//
// 与 english-podcast 的分段点播器同一条管线血统（HTTP → 环形缓冲 →
// esp_audio_simple_dec MP3 软解 → ES8311），但直播流没有「集」的概念：
// 单个 URL 无限读，读到断线就重连；没有断点续传，没有播放完成。
#pragma once

#include <cstddef>
#include <cstdint>

#include "mr_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

// C 侧也用的最小快照（UI 的 radio_ui_set_station 回调）。
typedef struct {
    char name[48];
    char url[128];
    uint8_t cat;        // mr_cat_t
    uint16_t freq_decihz;
} mr_station_snapshot_t;

enum class RadioPlaybackState {
    Stopped,
    Connecting,
    Buffering,
    Playing,
    Reconnecting,
    Error,
};

bool radio_player_init();
void radio_player_set_network(bool connected);
// 进配网模式前调用：停掉在跑的会话并释放预取环，把内存让给配网门户。
void radio_player_release_ring();
void radio_player_toggle();
void radio_player_set_playing(bool playing);
bool radio_player_is_playing();
bool radio_player_wait_idle(uint32_t timeout_ms);
void radio_player_select_relative(int delta);
void radio_player_set_station(std::size_t index);

// 本次连续收听时长（按已解码样本推进，暂停/缓冲时停住）。
uint32_t radio_player_listen_ms();
// 解码器报告的流格式（状态栏显示用；0 = 尚未解出）。
uint32_t radio_player_stream_hz();

std::size_t radio_player_station_index();
std::size_t radio_player_station_count();
mr_station_snapshot_t radio_player_station(std::size_t index);

// ---- 音量（NVS 持久化）----
void radio_player_set_volume(uint8_t volume);
uint8_t radio_player_volume();

// ---- 收藏（位图，NVS 持久化）----
uint32_t radio_player_favorites();
void radio_player_set_favorite(std::size_t index, bool on);

// ---- VU 律动条 ----
// 播放器在解码线程持续刷新 18 根能级条（0-100）；UI 以 100ms 轮询拉取。
// 返回本次填充的条数。beyond_playing=true 时条目保持最后一次的值，
// UI 在停止态自己做衰减。
#define MR_LEVEL_COUNT 18
uint8_t radio_player_levels(uint8_t *levels, uint8_t capacity);

#ifdef __cplusplus
}
#endif
