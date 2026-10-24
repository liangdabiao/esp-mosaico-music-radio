// main/mr_radio.c —— 见 mr_radio.h。
//
// 台源：蜻蜓 FM（qtfm.cn）音乐分类。清单由上游参考工程（FoloToy
// music-radio，2026-10-23 实测）取得；本仓库 2026-10-24 复测 18/18 全活。
#include "mr_radio.h"

const mr_station_t MR_STATIONS[MR_STATION_COUNT] = {
    // ---- 流行 87.5–93 MHz ----
    { "动感101",   "http://lhttp.qtfm.cn/live/274/64k.mp3",      MR_CAT_POP,     876 },
    { "LoveRadio", "http://lhttp.qtfm.cn/live/273/64k.mp3",      MR_CAT_POP,     886 },
    { "动听音乐",  "http://lhttp.qtfm.cn/live/5022107/64k.mp3",  MR_CAT_POP,     896 },
    { "星河音乐",  "http://lhttp.qtfm.cn/live/20210755/64k.mp3", MR_CAT_POP,     906 },
    { "清晨音乐",  "http://lhttp.qtfm.cn/live/4915/64k.mp3",     MR_CAT_POP,     916 },
    // ---- 经典 93–98 MHz ----
    { "怀旧好声音", "http://lhttp.qtfm.cn/live/1223/64k.mp3",     MR_CAT_CLASSIC, 933 },
    { "江苏经典",  "http://lhttp.qtfm.cn/live/4938/64k.mp3",      MR_CAT_CLASSIC, 948 },
    { "两广音乐",  "http://lhttp.qtfm.cn/live/20500149/64k.mp3",  MR_CAT_CLASSIC, 963 },
    // ---- 亚洲 98–103 MHz ----
    { "HD音乐",    "http://lhttp.qtfm.cn/live/15318341/64k.mp3",  MR_CAT_ASIA,    982 },
    { "亚洲天空",  "http://lhttp.qtfm.cn/live/20071/64k.mp3",     MR_CAT_ASIA,    992 },
    { "亚洲粤语",  "http://lhttp.qtfm.cn/live/15318569/64k.mp3",  MR_CAT_ASIA,    1002 },
    { "亚洲经典",  "http://lhttp.qtfm.cn/live/5021912/64k.mp3",   MR_CAT_ASIA,    1012 },
    // ---- 省市 103–108 MHz ----
    { "广东音乐",  "http://lhttp.qtfm.cn/live/1260/64k.mp3",      MR_CAT_LOCAL,   1033 },
    { "顺德音乐",  "http://lhttp.qtfm.cn/live/20500150/64k.mp3",  MR_CAT_LOCAL,   1041 },
    { "怀集音乐",  "http://lhttp.qtfm.cn/live/4804/64k.mp3",      MR_CAT_LOCAL,   1049 },
    { "苏州音乐",  "http://lhttp.qtfm.cn/live/2803/64k.mp3",      MR_CAT_LOCAL,   1057 },
    { "河北音乐",  "http://lhttp.qtfm.cn/live/1649/64k.mp3",      MR_CAT_LOCAL,   1065 },
    { "哈尔滨音乐", "http://lhttp.qtfm.cn/live/839/64k.mp3",      MR_CAT_LOCAL,   1073 },
};

_Static_assert(sizeof(MR_STATIONS) / sizeof(MR_STATIONS[0]) == MR_STATION_COUNT,
               "清单数量与头文件钉死的 MR_STATION_COUNT 不一致");

uint8_t mr_station_next(const uint8_t index)
{
    if (MR_STATION_COUNT == 0) return 0;
    if (index >= MR_STATION_COUNT - 1u) return 0;
    return (uint8_t)(index + 1u);
}

uint8_t mr_station_prev(const uint8_t index)
{
    if (MR_STATION_COUNT == 0) return 0;
    if (index == 0) return (uint8_t)(MR_STATION_COUNT - 1u);
    return (uint8_t)(index - 1u);
}

const char *mr_cat_name(const uint8_t cat)
{
    static const char *const NAMES[] = {
        [MR_CAT_POP] = "流行",
        [MR_CAT_CLASSIC] = "经典",
        [MR_CAT_ASIA] = "亚洲",
        [MR_CAT_LOCAL] = "省市",
        [MR_TAB_FAVORITES] = "收藏",
    };
    if (cat >= MR_TAB_COUNT) return "";
    return NAMES[cat];
}

bool mr_station_valid(const uint8_t index)
{
    return MR_STATION_COUNT > 0 && index < MR_STATION_COUNT;
}

bool mr_is_favorite(const uint32_t favorites, const uint8_t index)
{
    return index < MR_STATION_COUNT && (favorites & (1u << index)) != 0;
}

uint32_t mr_toggle_favorite(const uint32_t favorites, const uint8_t index)
{
    if (index >= MR_STATION_COUNT) return favorites;
    return favorites ^ (1u << index);
}

bool mr_favorite_at(const uint32_t favorites, const uint8_t fav_slot,
                    uint8_t *index_out)
{
    if (index_out == NULL) return false;
    uint8_t seen = 0;
    for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
        if (!mr_is_favorite(favorites, i)) continue;
        if (seen == fav_slot) {
            *index_out = i;
            return true;
        }
        ++seen;
    }
    return false;
}
