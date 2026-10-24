// main/radio_ui.h —— 触摸界面接口（LVGL 9.5，480×480）。
#pragma once

#include <cstddef>
#include <cstdint>

#include "radio_player.h"

#ifdef __cplusplus
enum class RadioPage {
    Tuner,          // 收音机主页（调谐盘）
    Stations,       // 电台列表（分组 + 收藏）
    Settings,
    WifiList,
    WifiKeyboard,
    WifiConnecting,
};

bool radio_ui_init();

// 按钮/事件回调统一入口（定义在 radio_ui.cc）。
void radio_ui_handle_action(uintptr_t action);

// ---- main.cc 提供的配网请求钩子 ----
void radio_wifi_request_scan();
void radio_wifi_request_connect(const char *ssid, const char *password);
void radio_wifi_request_cancel();

// ---- 播放器/网络状态推入（跨任务；内部自带 LVGL 锁纪律）----
void radio_ui_set_station(std::size_t index, std::size_t count,
                          const mr_station_snapshot_t &station);
void radio_ui_set_playback(RadioPlaybackState state, const char *detail);
void radio_ui_set_volume(uint8_t volume);
void radio_ui_set_favorites(uint32_t favorites);
void radio_ui_set_network(bool connected, const char *ssid);
void radio_ui_set_wifi_scan_results(const char (*ssids)[33], std::size_t count,
                                    bool scanning);
void radio_ui_set_wifi_connect_text(const char *text);

// ---- 页面跳转（main.cc / 事件回调）----
void radio_ui_show_tuner();
void radio_ui_show_stations();
void radio_ui_show_settings();
void radio_ui_show_wifi_list();
void radio_ui_show_wifi_keyboard(const char *ssid);
void radio_ui_show_wifi_connecting(const char *ssid);
RadioPage radio_ui_current_page();
#endif
