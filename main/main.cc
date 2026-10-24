// main/main.cc —— 音乐电台（ESP-Mosaico 版）入口。
//
// 硬件：esp32s31 + CO5300 480×480 触摸屏 + ES8311。
// 内容：蜻蜓 FM 18 个音乐直播流（见 mr_radio.c），清单固化在固件里。
// 配网：esp-wifi-connect（AP + 扫描 + 触摸键盘），english-podcast 真机同款；
//       配置成功组件自动重启进播放模式。
#include "bsp/display.h"
#include "esp_log.h"
extern "C" {
#include "iris_ota_support.h"
}
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "radio_clock.h"
#include "radio_display_settings.h"
#include "radio_player.h"
#include "radio_sleep_timer.h"
#include "radio_ui.h"

#include "ssid_manager.h"
#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

static const char *TAG = "music_radio";

namespace {

constexpr std::size_t kWifiMaxRows = 6;  // 与 radio_ui.cc 的列表行数一致

std::atomic<bool> s_connected{false};
std::atomic<bool> s_config_mode{false};
TaskHandle_t s_wifi_scan_task = nullptr;
TaskHandle_t s_wifi_connect_task = nullptr;
std::mutex s_wifi_ui_mutex;
char s_wifi_ssids[kWifiMaxRows][33] = {};
std::size_t s_wifi_ssid_count = 0;
std::string s_last_ssid;

void start_wifi_scan_ui();

// ---- 配网：扫描（english-podcast 同款）----

void wifi_scan_task(void *) {
    // GetConfigAccessPoints 拿的是配置门户缓存的扫描结果，门户自己也在周期
    // 扫描 —— 轮询至多 16×750 ms 总能等到一轮结果。
    for (int attempt = 0; attempt < 16 && s_config_mode.load(); ++attempt) {
        auto access_points = WifiManager::GetInstance().GetConfigAccessPoints();
        if (!access_points.empty()) {
            std::sort(access_points.begin(), access_points.end(),
                      [](const wifi_ap_record_t &left, const wifi_ap_record_t &right) {
                          return left.rssi > right.rssi;
                      });
            char ssids[kWifiMaxRows][33] = {};
            std::size_t count = 0;
            for (const auto &access_point : access_points) {
                const char *ssid = reinterpret_cast<const char *>(access_point.ssid);
                if (!ssid[0]) continue;
                bool duplicate = false;
                for (std::size_t i = 0; i < count; ++i) {
                    if (std::strncmp(ssids[i], ssid, sizeof(ssids[i])) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate || count >= kWifiMaxRows) continue;
                std::snprintf(ssids[count], sizeof(ssids[count]), "%.*s", 32, ssid);
                ++count;
            }
            if (count) {
                {
                    std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                    std::memcpy(s_wifi_ssids, ssids, sizeof(ssids));
                    s_wifi_ssid_count = count;
                }
                radio_ui_set_wifi_scan_results(s_wifi_ssids, count, false);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    bool show_empty = false;
    if (s_config_mode.load()) {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        show_empty = s_wifi_ssid_count == 0;
    }
    if (show_empty && s_config_mode.load()) {
        radio_ui_set_wifi_scan_results(s_wifi_ssids, 0, false);
    }
    s_wifi_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_scan_ui() {
    if (s_wifi_scan_task) return;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_wifi_ssid_count = 0;
    }
    radio_ui_set_wifi_scan_results(s_wifi_ssids, 0, true);
    if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096, nullptr, 4,
                    &s_wifi_scan_task) != pdPASS) {
        s_wifi_scan_task = nullptr;
        radio_ui_set_wifi_scan_results(s_wifi_ssids, 0, false);
    }
}

// ---- 配网：连接（english-podcast 同款）----

void wifi_connect_task(void *param) {
    char *args = static_cast<char *>(param);
    const std::string ssid(args);
    const std::string password(args + std::strlen(args) + 1);
    free(args);
    // ConfigureNetwork 阻塞验证密码（内部两次尝试 × 最长 10-25 s），成功时
    // 自己把 SSID 存进 NVS 并安排 2 秒后重启。
    const bool ok = WifiManager::GetInstance().ConfigureNetwork(ssid, password);
    if (ok) {
        radio_ui_set_wifi_connect_text("配置完成，即将重启");
    } else if (s_config_mode.load()) {
        radio_ui_set_wifi_connect_text("连接失败，请重试");
        if (radio_ui_current_page() == RadioPage::WifiConnecting) {
            radio_ui_show_wifi_keyboard(ssid.c_str());
        }
    }
    s_wifi_connect_task = nullptr;
    vTaskDelete(nullptr);
}

// ---- WiFi 事件 ----

bool on_wifi_page() {
    const RadioPage page = radio_ui_current_page();
    return page == RadioPage::WifiList || page == RadioPage::WifiKeyboard ||
           page == RadioPage::WifiConnecting;
}

void wifi_event(WifiEvent event, const std::string &data) {
    auto &manager = WifiManager::GetInstance();
    switch (event) {
        case WifiEvent::Scanning:
            ESP_LOGI(TAG, "wifi scanning");
            break;
        case WifiEvent::Connecting:
            ESP_LOGI(TAG, "wifi connecting: %s", data.c_str());
            break;
        case WifiEvent::Connected: {
            ESP_LOGI(TAG, "Wi-Fi connected: %s, IP=%s", data.c_str(),
                     manager.GetIpAddress().c_str());
            s_connected.store(true);
            s_config_mode.store(false);
            s_last_ssid = data;
            radio_ui_set_network(true, data.c_str());
            if (on_wifi_page()) radio_ui_show_tuner();
            manager.SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
            radio_clock_start();
            break;
        }
        case WifiEvent::Disconnected:
            s_connected.store(false);
            radio_player_set_network(false);
            radio_ui_set_network(false, "网络已断开");
            break;
        case WifiEvent::ConfigModeEnter: {
            s_config_mode.store(true);
            s_connected.store(false);
            radio_player_set_network(false);
            // 门户的网络缓冲要吃内存：预取环让位（静态环只清不删）。
            radio_player_release_ring();
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_wifi_ssid_count = 0;
            }
            ESP_LOGI(TAG, "config AP up: %s", manager.GetApSsid().c_str());
            radio_ui_show_wifi_list();
            start_wifi_scan_ui();
            break;
        }
        case WifiEvent::ConfigModeExit: {
            s_config_mode.store(false);
            if (on_wifi_page()) radio_ui_show_tuner();
            radio_ui_set_network(false, "正在连接网络");
            if (!manager.IsConnected()) manager.StartStation();
            break;
        }
    }
}

// 30 秒连不上且不在配网模式 → 自动开门户。
void connection_watchdog_task(void *) {
    while (true) {
        int waiting_seconds = 0;
        while (!s_connected.load() && !s_config_mode.load() && waiting_seconds < 30) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ++waiting_seconds;
        }
        if (!s_connected.load() && !s_config_mode.load()) {
            ESP_LOGW(TAG, "no usable network after 30 seconds; opening setup portal");
            WifiManager::GetInstance().StartConfigAp();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

// ---- UI → main 的配网请求钩子（radio_ui.h 声明）----

void radio_wifi_request_scan() {
    if (!s_config_mode.load()) {
        // 扫描 API 只在配网（AP）模式下可用；起 AP 会触发 ConfigModeEnter
        // 事件，那里自动开始扫描并切到网络列表页。
        WifiManager::GetInstance().StartConfigAp();
        return;
    }
    start_wifi_scan_ui();
}

void radio_wifi_request_connect(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || s_wifi_connect_task) return;
    const char *pass = password ? password : "";
    const std::size_t ssid_len = std::strlen(ssid);
    const std::size_t pass_len = std::strlen(pass);
    char *args = static_cast<char *>(malloc(ssid_len + pass_len + 2));
    if (!args) {
        radio_ui_set_wifi_connect_text("内存不足，请重试");
        return;
    }
    std::memcpy(args, ssid, ssid_len + 1);
    std::memcpy(args + ssid_len + 1, pass, pass_len + 1);
    if (xTaskCreate(wifi_connect_task, "wifi_join", 5120, args, 5,
                    &s_wifi_connect_task) != pdPASS) {
        free(args);
        s_wifi_connect_task = nullptr;
        radio_ui_set_wifi_connect_text("任务创建失败，请重试");
    }
}

void radio_wifi_request_cancel() {
    if (SsidManager::GetInstance().GetSsidList().empty()) {
        // 没有已存网络时保留配置 AP：这是设备唯一能上网的途径。
        return;
    }
    WifiManager::GetInstance().StopConfigAp();  // ConfigModeExit 里会自动回连
}

extern "C" void app_main(void) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    // Retained Recovery 契约：应用启动即向 Recovery 上报健康。
    iris_ota_support_start();

    if (!radio_display_settings_init()) {
        ESP_LOGW(TAG, "display settings init failed");
    }

    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "cannot lock LVGL");
        return;
    }
    const bool ui_ok = radio_ui_init();
    bsp_display_unlock();
    if (!ui_ok) {
        ESP_LOGE(TAG, "UI init failed");
        return;
    }

    if (!radio_player_init()) {
        ESP_LOGE(TAG, "player init failed");
        return;
    }
    if (!radio_sleep_timer_init()) {
        ESP_LOGW(TAG, "sleep timer init failed");
    }

    auto &manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "Music Radio";
    config.fixed_ap_ssid = "Music Radio";
    config.language = "zh-CN";
    config.station_hostname = "music-radio";
    config.station_scan_min_interval_seconds = 5;
    config.station_scan_max_interval_seconds = 30;
    if (!manager.Initialize(config)) {
        ESP_LOGE(TAG, "wifi init failed");
        return;
    }
    manager.SetEventCallback(wifi_event);

    if (SsidManager::GetInstance().GetSsidList().empty()) {
        // 首配：直接开热点「Music Radio」，手机连上后在触摸屏上选网输密码。
        manager.StartConfigAp();
    } else {
        manager.StartStation();
    }

    if (xTaskCreate(connection_watchdog_task, "wifi_guard", 3072, nullptr, 4,
                    nullptr) != pdPASS) {
        ESP_LOGW(TAG, "wifi watchdog task failed");
    }
    ESP_LOGI(TAG, "ready: 480x480 touch, live streaming");
}
