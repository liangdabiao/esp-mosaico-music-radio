// main/radio_clock.h —— SNTP 墙上时钟（联网后自动同步，断网退化为空）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 连网成功后调用一次（幂等）。启动 SNTP，时区固定东八区。
void radio_clock_start(void);

// 是否已经从 NTP 拿到过有效时间。
bool radio_clock_synced(void);

// "HH:MM"（24 小时制）；未同步时返回 false 且清空 out。
bool radio_clock_hhmm(char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
