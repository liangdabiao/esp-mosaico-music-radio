// main/radio_clock.c —— SNTP 墙上时钟。
//
// 收音机顶栏要一块钟。esp_netif_sntp 简单封装：联网后 start 一次，
// 之后 UI 随时读。同步成功一次就置位（之后系统时钟自己走，不再依赖网络）。
#include "radio_clock.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "radio_clock";

static bool s_started;
static volatile bool s_synced;

static void time_sync_notification(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "time synchronized");
}

void radio_clock_start(void)
{
    if (s_started) return;
    s_started = true;

    // 东八区（无夏令时）；在 init 前设置，localtime 直接可用。
    setenv("TZ", "CST-8", 1);
    tzset();

    // 多服务器兜底（sdkconfig.defaults 里 CONFIG_LWIP_SNTP_MAX_SERVERS=3）。
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com",
                                "cn.pool.ntp.org"));
    config.sync_cb = time_sync_notification;
    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
        s_started = false;
    }
}

bool radio_clock_synced(void)
{
    return s_synced;
}

bool radio_clock_hhmm(char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) return false;
    out[0] = '\0';
    if (!s_synced) return false;

    time_t now = 0;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year < 2020 - 1900) return false;   // 明显还没同步过
    snprintf(out, capacity, "%02d:%02d", local.tm_hour, local.tm_min);
    return true;
}
