// tests/test_mr_radio.c —— mr_radio 纯 C11 宿主测试（不依赖 IDF）。
//
// 编译（Git Bash）：
//   gcc -std=c11 -Wall -Wextra -I main tests/test_mr_radio.c main/mr_radio.c -o tests/test_mr_radio.exe
//   ./tests/test_mr_radio.exe
#include "mr_radio.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++failures;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static void test_station_table(void)
{
    CHECK(MR_STATION_COUNT == 18);
    for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
        const mr_station_t *st = &MR_STATIONS[i];
        CHECK(st->name[0] != '\0');
        // 蜻蜓直播流的 URL 形态；写错一个字母设备上就是 404。
        CHECK(strncmp(st->url, "http://lhttp.qtfm.cn/live/", 26) == 0);
        CHECK(strncmp(st->url + strlen(st->url) - 8, "/64k.mp3", 8) == 0);
        CHECK(st->cat < MR_CAT_COUNT);
        CHECK(st->freq_decihz >= 875 && st->freq_decihz <= 1080);
    }
    // 台名唯一（NVS 按名字恢复上次收听，重名会串台）。
    for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
        for (uint8_t j = (uint8_t)(i + 1); j < MR_STATION_COUNT; ++j) {
            CHECK(strcmp(MR_STATIONS[i].name, MR_STATIONS[j].name) != 0);
        }
    }
    // 调谐位置唯一（刻度盘上台标不重叠）。
    for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
        for (uint8_t j = (uint8_t)(i + 1); j < MR_STATION_COUNT; ++j) {
            CHECK(MR_STATIONS[i].freq_decihz != MR_STATIONS[j].freq_decihz);
        }
    }
}

static void test_wrapping(void)
{
    CHECK(mr_station_next(MR_STATION_COUNT - 1) == 0);
    CHECK(mr_station_prev(0) == MR_STATION_COUNT - 1);
    CHECK(mr_station_next(0) == 1);
    CHECK(mr_station_prev(1) == 0);
    CHECK(!mr_station_valid(MR_STATION_COUNT));
    CHECK(mr_station_valid(0));
}

static void test_favorites(void)
{
    uint32_t favorites = 0;
    CHECK(!mr_is_favorite(favorites, 0));
    favorites = mr_toggle_favorite(favorites, 3);
    CHECK(mr_is_favorite(favorites, 3));
    CHECK(!mr_is_favorite(favorites, 2));
    favorites = mr_toggle_favorite(favorites, 17);
    CHECK(mr_is_favorite(favorites, 17));
    uint8_t index = 0;
    CHECK(mr_favorite_at(favorites, 0, &index) && index == 3);
    CHECK(mr_favorite_at(favorites, 1, &index) && index == 17);
    CHECK(!mr_favorite_at(favorites, 2, &index));
    favorites = mr_toggle_favorite(favorites, 3);
    CHECK(!mr_is_favorite(favorites, 3));
    CHECK(mr_favorite_at(favorites, 0, &index) && index == 17);
    // 越界不崩、不变。
    CHECK(mr_toggle_favorite(favorites, 200) == favorites);
    CHECK(!mr_is_favorite(favorites, 200));
}

static void test_cat_names(void)
{
    CHECK(strcmp(mr_cat_name(MR_CAT_POP), "流行") == 0);
    CHECK(strcmp(mr_cat_name(MR_CAT_CLASSIC), "经典") == 0);
    CHECK(strcmp(mr_cat_name(MR_CAT_ASIA), "亚洲") == 0);
    CHECK(strcmp(mr_cat_name(MR_CAT_LOCAL), "省市") == 0);
    CHECK(strcmp(mr_cat_name(MR_TAB_FAVORITES), "收藏") == 0);
    CHECK(strcmp(mr_cat_name(99), "") == 0);
}

// 每组至少一台；收藏过滤闭环依赖它。
static void test_group_coverage(void)
{
    uint32_t seen = 0;
    for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) seen |= (1u << MR_STATIONS[i].cat);
    CHECK(seen == ((1u << MR_CAT_POP) | (1u << MR_CAT_CLASSIC) |
                   (1u << MR_CAT_ASIA) | (1u << MR_CAT_LOCAL)));
}

int main(void)
{
    test_station_table();
    test_wrapping();
    test_favorites();
    test_cat_names();
    test_group_coverage();
    if (failures == 0) {
        printf("mr_radio: all tests passed\n");
        return 0;
    }
    printf("mr_radio: %d failure(s)\n", failures);
    return 1;
}
