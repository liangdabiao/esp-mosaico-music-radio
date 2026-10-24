// main/mr_radio.h —— 内置电台清单、分类与收藏（蜻蜓 FM 音乐台精选）。
//
// 清单固化在固件里（v1 不做在线搜台）。每个地址都是 2026-10-24 实测
// （纯 http GET 收流 200，18/18 台全活）；流格式统一为 MPEG1 L3
// 44.1 kHz joint-stereo 64 kbps，播放端按 8 KB/s 预算缓冲。
//
// 本模块与 ESP-IDF/LVGL/播放器解耦（纯 C11），宿主测试直接编译它。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MR_CAT_POP = 0,    // 流行
    MR_CAT_CLASSIC,    // 经典
    MR_CAT_ASIA,       // 亚洲
    MR_CAT_LOCAL,      // 省市
    MR_CAT_COUNT,
    // 列表页的第五个「分组」：收藏（跨组的星标过滤，不是电台类别）。
    MR_TAB_FAVORITES,
    MR_TAB_COUNT,
} mr_cat_t;

typedef struct {
    const char *name;  // 短名：全角 ≤7 字或半角 ≤13 字符（32px 一行硬保证）
    const char *url;   // 蜻蜓直播流（纯 http MP3）
    uint8_t cat;       // mr_cat_t
    // 调谐刻度盘上的位置，单位 0.1 MHz。台与台不重合即可：四组分段落在
    // 87.5–108 MHz 频段上（流行 87.5-93 / 经典 93-98 / 亚洲 98-103 /
    // 省市 103-108），组内按清单顺序均匀铺开 —— 复古调谐盘的观感成立，
    // 不冒充真实播出频率（ internet 流没有 FM 频率一说）。
    uint16_t freq_decihz;
} mr_station_t;

#define MR_STATION_COUNT 18
extern const mr_station_t MR_STATIONS[MR_STATION_COUNT];

uint8_t mr_station_next(uint8_t index);
uint8_t mr_station_prev(uint8_t index);
const char *mr_cat_name(uint8_t cat);
bool mr_station_valid(uint8_t index);

// ---- 收藏 ----
// 位图持久化由调用方（radio_player）负责；这里只做位运算与容量判断。
bool mr_is_favorite(uint32_t favorites, uint8_t index);
uint32_t mr_toggle_favorite(uint32_t favorites, uint8_t index);
// 第 fav_slot 个收藏电台的索引；没有那么多收藏时返回 false。
bool mr_favorite_at(uint32_t favorites, uint8_t fav_slot, uint8_t *index_out);

#ifdef __cplusplus
}
#endif
