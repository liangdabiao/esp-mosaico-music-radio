#ifndef _WIFI_CONFIGURATION_AP_H_
#define _WIFI_CONFIGURATION_AP_H_

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

#include "dns_server.h"
#include "sdkconfig.h"

/**
 * WifiConfigurationAp - WiFi configuration access point
 * 
 * Creates a WiFi hotspot with a captive portal for configuring WiFi credentials.
 * Note: WiFi driver must be initialized before using this class.
 */
class WifiConfigurationAp {
public:
    WifiConfigurationAp();
    ~WifiConfigurationAp();

    // Delete copy constructor and assignment operator
    WifiConfigurationAp(const WifiConfigurationAp&) = delete;
    WifiConfigurationAp& operator=(const WifiConfigurationAp&) = delete;

    void SetSsidPrefix(const std::string &&ssid_prefix);
    void SetSsidPrefix(const std::string &ssid_prefix);
    void SetFixedSsid(const std::string &ssid) { fixed_ssid_ = ssid; }
    void SetLanguage(const std::string &&language);
    void SetLanguage(const std::string &language);
    void SetShowOtaConfig(bool show) { show_ota_config_ = show; }
    void SetShowSleepConfig(bool show) { show_sleep_config_ = show; }
    void Start();
    void Stop();
#if !CONFIG_IDF_TARGET_ESP32P4
    void StartSmartConfig();
#endif
    bool ConnectToWifi(const std::string &ssid, const std::string &password);
    void Save(const std::string &ssid, const std::string &password);
    std::vector<wifi_ap_record_t> GetAccessPoints();
    std::string GetSsid();
    std::string GetWebServerUrl();

    /**
     * 配网结束（成功提交或手动退出）后延迟重启设备。
     *
     * 为什么重启而不是热切换：门户（热点+网页+DNS，要 ~35 KB 空闲堆才能
     * 正常发网页）和流式播放（MP3 解码器内部要一整块 ~20 KB 连续内存）
     * 在这台设备上共用一片小堆，门户一开一关后堆必然碎片化，解码器那块
     * 连续内存再也拼不出来 —— 真机实测（2026-10-01）热切换后 80 秒内
     * 解码持续失败。干净启动时两者都宽裕，所以把「配网完成」做成一次
     * 约 7 秒的自动重启：用户提交后设备自己重启、自动连网并开始播放。
     */
    void ScheduleConfigExitRestart();

    /**
     * Set callback for when exit is requested from config mode
     * This is called when user requests to exit config mode (e.g., via /exit endpoint)
     */
    void OnExitRequested(std::function<void()> callback);

private:
    std::mutex mutex_;
    std::unique_ptr<DnsServer> dns_server_;
    httpd_handle_t server_ = NULL;
    EventGroupHandle_t event_group_;
    std::string ssid_prefix_;
    std::string fixed_ssid_;
    std::string language_;
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    esp_timer_handle_t scan_timer_ = nullptr;
    bool is_connecting_ = false;
    esp_netif_t* ap_netif_ = nullptr;
    std::vector<wifi_ap_record_t> ap_records_;

    // 高级配置项
    std::string ota_url_;
    int8_t max_tx_power_;
    bool remember_bssid_;
    bool sleep_mode_;
    bool show_ota_config_ = false;
    bool show_sleep_config_ = false;

    // Callbacks
    std::function<void()> on_exit_requested_;

    void StartAccessPoint();
    void StartWebServer();

    // Event handlers
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
#if !CONFIG_IDF_TARGET_ESP32P4
    static void SmartConfigEventHandler(void* arg, esp_event_base_t event_base, 
                                      int32_t event_id, void* event_data);
    esp_event_handler_instance_t sc_event_instance_ = nullptr;
#endif
};

#endif // _WIFI_CONFIGURATION_AP_H_
