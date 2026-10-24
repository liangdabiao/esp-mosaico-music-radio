idf_component_register(
    SRCS "main.cc"
         # 播放器（直播流：HTTP → 环形缓冲 → MP3 软解 → ES8311）
         "radio_player.cc"
         "radio_ui.cc"
         # 电台表（纯 C11，宿主测试直接编译）
         "mr_radio.c"
         # 服务
         "radio_clock.c"
         "radio_sleep_timer.cc"
         "radio_display_settings.cc"
    INCLUDE_DIRS "."
    # BSP（显示/触摸/音频）、LVGL、codec、NVS + 网络栈（Wi-Fi/HTTP/TLS 证书包）。
    # esp_audio_codec（MP3 解码）/ esp-wifi-connect / cjson 由 components/ 提供。
    REQUIRES esp-mosaico-bsp esp_lvgl_adapter lvgl esp_codec_dev
             esp_mosaico_app_recovery
             nvs_flash esp_timer esp_event esp_hw_support
             esp_wifi esp_netif esp_http_client esp-tls mbedtls
             esp_audio_codec esp-wifi-connect cjson
)

# 中文/拉丁字库子集（16/24/32px）：由 tools/gen_font.py 从 main/ 的字面量
# 推导字符清单，lv_font_conv + Noto Sans CJK SC 烘焙。改界面文案后必须重跑
# 那个脚本，否则新字在设备上渲染成乱码（清单见 assets/fonts/charset.txt）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/mr_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/mr_font_24.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/mr_font_32.c"
)
