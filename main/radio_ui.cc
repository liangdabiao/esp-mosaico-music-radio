// main/radio_ui.cc —— 触摸界面（CO5300 480×480，LVGL 9.5，霓虹靛蓝主题）。
//
// 页面：收音机（Tuner，调谐盘 + VU 律动条 + 音量弹层）→ 电台列表（分组 +
// 收藏星标）→ 设置；WiFi 配网三页（列表 → 密码键盘 → 连接中）与
// english-podcast 同款（真机验证过）。
//
// 「谁在什么时候重建屏幕」沿用 english-podcast 的血泪纪律：
//   * 所有整页重建走 lv_async_call 推到下一个 LVGL 周期，绝不在按钮事件
//     回调里 lv_obj_delete 当前屏幕；
//   * 只改文字/数值的更新一律原地做；VU 条与状态行由 200ms 同步定时器拉取；
//   * schedule_page_rebuild 自己取 LVGL 锁（可重入，回调里再取不会死锁）。
#include "radio_ui.h"

#include "mr_radio.h"
#include "radio_clock.h"
#include "radio_display_settings.h"
#include "radio_sleep_timer.h"

#include "bsp/display.h"
#include "esp_log.h"
#include "esp_system.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern const lv_font_t mr_font_16;
extern const lv_font_t mr_font_24;
extern const lv_font_t mr_font_32;

namespace {

constexpr char kTag[] = "radio_ui";
constexpr int kW = 480;
constexpr int kH = 480;

// ---- 霓虹靛蓝主题 ----
constexpr uint32_t kColorBg = 0x0D0F1E;       // 深夜靛蓝底
constexpr uint32_t kColorBgSoft = 0x151830;
constexpr uint32_t kColorCard = 0x1D2040;
constexpr uint32_t kColorCardHot = 0x2B2F58;
constexpr uint32_t kColorInk = 0xF1F2FA;
constexpr uint32_t kColorMuted = 0x878CB0;
constexpr uint32_t kColorAccent = 0x7B6BFF;   // 霓虹靛蓝
constexpr uint32_t kColorAccent2 = 0x4FD8C4;  // 青绿（VU 渐变端）
constexpr uint32_t kColorWarn = 0xE8B84B;
constexpr uint32_t kColorStar = 0xF5C542;
constexpr uint32_t kColorOnline = 0x53D97F;

// 分类 chip 的各自颜色（刻度盘上的台标也用它，一眼分出四组）。
constexpr uint32_t kCatColor[] = {
    0x7B6BFF,  // 流行 靛
    0xE8B84B,  // 经典 金
    0x4FD8C4,  // 亚洲 青
    0xFF8E72,  // 省市 橙
    0xF5C542,  // 收藏 星
};

// 控件动作编号。
enum : uintptr_t {
    kActShowTuner = 1,
    kActShowStations,
    kActShowSettings,
    kActTabBase,          // + i（0-4）：流行/经典/亚洲/省市/收藏
    kActStationPrev,
    kActStationNext,
    kActPlayToggle,
    kActVolOpen,
    kActVolDown,
    kActVolUp,
    kActDialPrev,         // 刻度盘左 1/3 轻点 = 上一台
    kActDialNext,         // 右 1/3 轻点 = 下一台
    kActPagePrev,
    kActPageNext,
    kActSleepBase,        // + i（0-4）：关/15/30/60/90
    kActBrightnessCycle,
    kActTimeoutCycle,
    kActStarToggle,       // user_data 另传（按钮回调各自绑，不走这里）
    // ---- WiFi 配网 ----
    kActWifiOpen,
    kActWifiRescan,
    kActWifiCancel,
    kActWifiBack,
    kActWifiConnect,
    kActWifiKeyMode,
    kActWifiSpace,
    kActWifiBackspace,
};

RadioPage s_page = RadioPage::Tuner;
lv_obj_t *s_screen = nullptr;
lv_timer_t *s_sync_timer = nullptr;

// 列表页分页：每页 5 张卡片。
constexpr std::size_t kRows = 5;
constexpr int kListTop = 106;
constexpr int kCardH = 62;
constexpr int kCardPitch = 68;
std::size_t s_list_page = 0;
uint8_t s_list_tab = 0;   // mr_cat_t 或 MR_TAB_FAVORITES

// 状态镜像。
char s_playback_text[48] = "";
char s_station_name[48] = "";
uint16_t s_station_freq = 0;
uint8_t s_station_cat = 0;
std::size_t s_station_index = 0;
uint8_t s_volume = 80;
uint32_t s_favorites = 0;
bool s_playing = false;

// 网络状态镜像。
bool s_network_up = false;
char s_network_ssid[32] = "";

// WiFi 配网状态（main 推入；照抄 english-podcast）。
constexpr std::size_t kWifiMaxRows = 6;
char s_wifi_ssids[kWifiMaxRows][33] = {};
std::size_t s_wifi_count = 0;
bool s_wifi_scanning = false;
char s_wifi_target[33] = {};
char s_wifi_pass[64] = {};
int s_wifi_key_mode = 0;

// WiFi SSID 高频汉字表：字模工具从字面量烘进字库（english-podcast 同款）。
static const char kWifiSsidCharset[] __attribute__((used)) =
    "的在一是有不了人我他这中大来上国个到时说们要学就生着你也子那得还自会"
    "家用可于下天又去想么吧没年看见只小发的问出几们前后多定行现成回点己月"
    "明长女交手什么事十从开面心高文力但身方实吃叫当住听打呢真全才四已所之"
    "最光情路分总条白东次亲如被花口放儿常气五第使写军运再果怎许快因别飞外"
    "树物活部无往船望新带先力完却站代员机更九您每风级跟笑啊孩万少直意夜比"
    "连车重便斗马哪化太指变社似士者干石满日决百原拿群究各六本思解立河村八"
    "难早论吗根共让相研今其书坐接应关信觉步反处记将千找争领或师结块跑谁草"
    "越字加脚紧爱等习阵怕青半火法题建赶位唱海七任件感准张团屋离色脸片科倒"
    "睛利世刚且由送切星导晚表够整认响雪流未场该并底深刻平伟忙提确近亮轻讲"
    "农古黑告界拉名呀土清阳照办史改历转画造嘴此治北必服雨穿内识验传业菜爬"
    "睡兴形量咱观苦体众通冲合破友度术饭公旁房极南枪读沙岁线野坚空收算至政"
    "城劳落钱特围弟胜教热展包歌类渐强数乡呼性音答哥际旧神念";

// ---- 页面上可原地改的控件（render_ 开头清空）----
lv_obj_t *s_clock_label = nullptr;
lv_obj_t *s_net_label = nullptr;
lv_obj_t *s_net_dot = nullptr;
lv_obj_t *s_name_label = nullptr;     // 台名（32px）
lv_obj_t *s_meta_label = nullptr;     // 分类 + 调谐位置
lv_obj_t *s_status_label = nullptr;   // 状态行
lv_obj_t *s_vu_bar[MR_LEVEL_COUNT] = {};
lv_obj_t *s_play_label = nullptr;
lv_obj_t *s_vol_label = nullptr;      // 主页/设置页音量数值
lv_obj_t *s_vol_bar = nullptr;
lv_obj_t *s_overlay = nullptr;        // 音量弹层
lv_obj_t *s_overlay_bar = nullptr;
lv_obj_t *s_overlay_label = nullptr;
lv_obj_t *s_wifi_status_label = nullptr;
lv_obj_t *s_row_tag[kRows] = {};      // 列表行「播放中/直播」标签
lv_obj_t *s_row_star[kRows] = {};
std::size_t s_row_index[kRows] = {};
lv_timer_t *s_overlay_timer = nullptr;

// VU 衰减镜像（停播时每 tick 向 0 收敛，播放中跟播放器实时值）。
uint8_t s_vu_display[MR_LEVEL_COUNT] = {};

void schedule_page_rebuild();
void hide_volume_overlay();
bool update_tuner_widgets();
bool update_station_widgets();

// ---- 基础控件工厂 ----

lv_obj_t *bg_screen() {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_size(scr, kW, kH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

lv_obj_t *label_make(lv_obj_t *parent, const char *text, const lv_font_t *font,
                     uint32_t color, int x, int y, int w) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    if (w > 0) {
        lv_obj_set_width(label, w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    return label;
}

lv_obj_t *center_label(lv_obj_t *label, int w) {
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

// 纯装饰矩形：无样式、不可点、不滚动（lv_obj 默认样式和 flag 会拦触摸、
// 出滚动条 —— 刻度盘上的刻度线/台标必须全部关掉）。
lv_obj_t *plain_rect(lv_obj_t *parent, int x, int y, int w, int h,
                     uint32_t color, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_clear_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                                     LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

lv_obj_t *button_make(lv_obj_t *parent, int x, int y, int w, int h,
                      const char *text, const lv_font_t *font,
                      uint32_t bg, uint32_t fg, uintptr_t action) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *label = label_make(btn, text, font, fg, 0, 0, 0);
    lv_obj_center(label);
    if (action) lv_obj_add_event_cb(btn, [](lv_event_t *event) {
        const uintptr_t act =
            reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
        radio_ui_handle_action(act);
    }, LV_EVENT_CLICKED, reinterpret_cast<void *>(action));
    return btn;
}

lv_obj_t *bar_make(lv_obj_t *parent, int x, int y, int w, int h, int32_t max,
                   uint32_t indicator) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_radius(bar, h / 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBgSoft), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(indicator), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, max);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

void reset_live_widgets() {
    s_clock_label = nullptr;
    s_net_label = nullptr;
    s_net_dot = nullptr;
    s_name_label = nullptr;
    s_meta_label = nullptr;
    s_status_label = nullptr;
    s_play_label = nullptr;
    s_vol_label = nullptr;
    s_vol_bar = nullptr;
    s_overlay = nullptr;
    s_overlay_bar = nullptr;
    s_overlay_label = nullptr;
    s_wifi_status_label = nullptr;
    for (int i = 0; i < MR_LEVEL_COUNT; ++i) s_vu_bar[i] = nullptr;
    for (int i = 0; i < static_cast<int>(kRows); ++i) {
        s_row_tag[i] = nullptr;
        s_row_star[i] = nullptr;
    }
}

void replace_screen(lv_obj_t *next) {
    if (s_screen) lv_obj_delete(s_screen);
    s_screen = next;
    lv_scr_load(next);
}

void fmt_mmss(char *out, std::size_t cap, uint32_t ms) {
    const uint32_t total_sec = ms / 1000;
    // 该工具链的 uint32_t 落在 unsigned long，printf 系列必须显式收窄。
    std::snprintf(out, cap, "%u:%02u",
                  static_cast<unsigned>(total_sec / 60),
                  static_cast<unsigned>(total_sec % 60));
}

const char *playback_state_text() {
    if (s_playback_text[0]) return s_playback_text;
    return s_network_up ? "就绪" : "未联网";
}

// 刻度盘坐标：0.1 MHz → x。频段 87.5–108.0 MHz 铺满 24..456。
int dial_x(uint16_t freq_decihz) {
    constexpr int kX0 = 24;
    constexpr int kX1 = 456;
    int f = freq_decihz;
    if (f < 875) f = 875;
    if (f > 1080) f = 1080;
    return kX0 + (f - 875) * (kX1 - kX0) / (1080 - 875);
}

// 顶栏（Tuner 页）：时钟 + 网络状态。
void render_top_bar(lv_obj_t *scr) {
    char hhmm[8] = "";
    radio_clock_hhmm(hhmm, sizeof(hhmm));
    s_clock_label = label_make(scr, hhmm[0] ? hhmm : "--:--", &mr_font_24,
                               kColorInk, 16, 12, 0);

    constexpr int kDotX = kW - 150;
    s_net_dot = plain_rect(scr, kDotX, 24, 10, 10,
                           s_network_up ? kColorOnline : kColorWarn, 5);
    char net_text[40];
    std::snprintf(net_text, sizeof(net_text), "%s",
                  s_network_up ? (s_network_ssid[0] ? s_network_ssid : "已连接")
                               : "未联网");
    s_net_label = label_make(scr, net_text, &mr_font_16, kColorMuted, 0, 20, 120);
    lv_obj_align(s_net_label, LV_ALIGN_TOP_RIGHT, -14, 0);
}

// 分组 chip 行（Tuner 与列表页共用；chips 数量 5）。
void render_tabs(lv_obj_t *scr, int y) {
    constexpr int kChipW = 84;
    constexpr int kGap = 8;
    const int x0 = (kW - (5 * kChipW + 4 * kGap)) / 2;
    for (int i = 0; i < static_cast<int>(MR_TAB_COUNT); ++i) {
        const bool active = (s_page == RadioPage::Tuner &&
                             i == MR_STATIONS[s_station_index].cat) ||
                            (s_page == RadioPage::Stations &&
                             static_cast<uint8_t>(i) == s_list_tab);
        button_make(scr, x0 + i * (kChipW + kGap), y, kChipW, 38,
                    mr_cat_name(static_cast<uint8_t>(i)), &mr_font_16,
                    active ? kCatColor[i] : kColorCard,
                    active ? kColorBg : kColorInk,
                    kActTabBase + static_cast<uintptr_t>(i));
    }
}

// ---- Tuner 页 ----

void render_dial(lv_obj_t *scr) {
    // 底衬
    lv_obj_t *panel = plain_rect(scr, 12, 100, kW - 24, 92, kColorBgSoft, 18);

    // 刻度：整 MHz 短线，每 5 MHz 长线。
    lv_obj_t *line = lv_line_create(panel);
    static lv_point_precise_t band_pts[2] = {{12, 50}, {kW - 36, 50}};
    lv_line_set_points(line, band_pts, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(kColorMuted), 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    for (int mhz = 88; mhz <= 108; ++mhz) {
        const int x = dial_x(static_cast<uint16_t>(mhz * 10)) - 12;
        const bool major = (mhz % 5 == 0);
        plain_rect(panel, x, 50 - (major ? 12 : 6), 2, major ? 12 : 6,
                   major ? kColorInk : kColorMuted, 1);
    }

    // 每台的圆点台标（按组配色；当前台更大更亮）。
    for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
        const int x = dial_x(MR_STATIONS[i].freq_decihz) - 12;
        const bool hot = (i == s_station_index);
        const int d = hot ? 14 : 8;
        plain_rect(panel, x - d / 2 + 1, 50 - d / 2, d, d,
                   hot ? kColorInk : kCatColor[MR_STATIONS[i].cat], d / 2);
    }

    // 指针：当前台位置的竖线。
    const int nx = dial_x(MR_STATIONS[s_station_index].freq_decihz) - 12;
    plain_rect(panel, nx - 1, 14, 3, 34, kColorAccent, 2);

    // 频段端点标注。
    label_make(panel, "87.5", &mr_font_16, kColorMuted, 4, 60, 0);
    label_make(panel, "108", &mr_font_16, kColorMuted, kW - 60, 60, 0);

    // 轻点盘：左 1/3 上一台，右 1/3 下一台（透明热区，保持可点）。
    const int zone_w = (kW - 24) / 3;
    const int dial_x0[2] = {12, 12 + 2 * zone_w};
    for (int z = 0; z < 2; ++z) {
        lv_obj_t *zone = lv_obj_create(scr);
        lv_obj_remove_style_all(zone);
        lv_obj_set_size(zone, zone_w, 92);
        lv_obj_set_pos(zone, dial_x0[z], 100);
        lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(zone, [](lv_event_t *event) {
            const uintptr_t act =
                reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
            radio_ui_handle_action(act);
        }, LV_EVENT_CLICKED,
           reinterpret_cast<void *>(z == 0 ? kActDialPrev : kActDialNext));
    }
}

void render_vu(lv_obj_t *scr) {
    // 18 根竖条：宽 12、间距 26，占满 454px。颜色沿 青→靛 渐变。
    constexpr int kBarW = 12;
    constexpr int kPitch = 26;
    const int x0 = (kW - (MR_LEVEL_COUNT * kPitch - (kPitch - kBarW))) / 2;
    const int y0 = 306;
    const int kBarH = 58;
    for (int i = 0; i < MR_LEVEL_COUNT; ++i) {
        const float t = static_cast<float>(i) / (MR_LEVEL_COUNT - 1);
        const uint32_t r = static_cast<uint32_t>(0x4F + (0x7B - 0x4F) * t);
        const uint32_t g = static_cast<uint32_t>(0xD8 + (0x6B - 0xD8) * t);
        const uint32_t b = static_cast<uint32_t>(0xC4 + (0xFF - 0xC4) * t);
        lv_obj_t *bar = lv_bar_create(scr);
        lv_obj_set_pos(bar, x0 + i * kPitch, y0);
        lv_obj_set_size(bar, kBarW, kBarH);
        lv_obj_set_style_radius(bar, 5, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBgSoft), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex((r << 16) | (g << 8) | b),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        s_vu_bar[i] = bar;
    }
}

void render_volume_overlay(lv_obj_t *scr) {
    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, 300, 132);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x232750), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_overlay, 20, 0);
    lv_obj_set_style_border_width(s_overlay, 2, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = label_make(s_overlay, "音量", &mr_font_16, kColorMuted, 0, 12, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
    s_overlay_label = center_label(
        label_make(s_overlay, "", &mr_font_32, kColorInk, 0, 34, 0), 300);
    s_overlay_bar = bar_make(s_overlay, 40, 96, 220, 10, 100, kColorAccent);

    lv_obj_t *minus = button_make(s_overlay, 12, 82, 48, 38, "−", &mr_font_24,
                                  kColorCard, kColorInk, kActVolDown);
    (void)minus;
    lv_obj_t *plus = button_make(s_overlay, 240, 82, 48, 38, "+", &mr_font_24,
                                 kColorCard, kColorInk, kActVolUp);
    (void)plus;
}

void render_tuner() {
    reset_live_widgets();
    lv_obj_t *scr = bg_screen();

    render_top_bar(scr);
    render_tabs(scr, 54);
    render_dial(scr);

    s_name_label = center_label(
        label_make(scr, s_station_name, &mr_font_32, kColorInk, 0, 214, 0), kW);
    char meta[64];
    std::snprintf(meta, sizeof(meta), "%s · 调谐 %u.%u", mr_cat_name(s_station_cat),
                  s_station_freq / 10, s_station_freq % 10);
    s_meta_label = center_label(
        label_make(scr, meta, &mr_font_16, kCatColor[s_station_cat], 0, 258, 0), kW);

    s_status_label = center_label(
        label_make(scr, playback_state_text(), &mr_font_16, kColorMuted, 0, 278, 0), kW);

    render_vu(scr);
    render_volume_overlay(scr);

    // 控制行：[电台列表][‹][播放][›][音量]
    constexpr int kCtlY = 384;
    constexpr int kCtlH = 80;
    button_make(scr, 8, kCtlY, 86, kCtlH, "电台\n列表", &mr_font_16, kColorCard,
                kColorInk, kActShowStations);
    button_make(scr, 102, kCtlY, 72, kCtlH, "‹", &mr_font_32, kColorCard,
                kColorInk, kActStationPrev);
    lv_obj_t *play = button_make(scr, 182, kCtlY, 100, kCtlH,
                                 s_playing ? "暂停" : "播放", &mr_font_24,
                                 kColorAccent, kColorBg, kActPlayToggle);
    lv_obj_set_style_radius(play, 32, 0);
    s_play_label = lv_obj_get_child(play, 0);
    button_make(scr, 290, kCtlY, 72, kCtlH, "›", &mr_font_32, kColorCard,
                kColorInk, kActStationNext);
    button_make(scr, 370, kCtlY, 86, kCtlH, "音量", &mr_font_16, kColorCard,
                kColorInk, kActVolOpen);

    replace_screen(scr);
    ESP_LOGI(kTag, "tuner: station=%u net=%d free heap %u KB",
             static_cast<unsigned>(s_station_index), s_network_up ? 1 : 0,
             static_cast<unsigned>(esp_get_free_heap_size() / 1024));
}

// ---- 电台列表页 ----

void collect_tab_indices(uint8_t *out, std::size_t *count) {
    std::size_t n = 0;
    if (s_list_tab == MR_TAB_FAVORITES) {
        for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
            if (mr_is_favorite(s_favorites, i)) out[n++] = i;
        }
    } else {
        for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
            if (MR_STATIONS[i].cat == s_list_tab) out[n++] = i;
        }
    }
    *count = n;
}

void star_clicked(void *index_ptr) {
    const std::size_t index = reinterpret_cast<std::size_t>(index_ptr);
    radio_player_set_favorite(index, !mr_is_favorite(s_favorites,
                                                     static_cast<uint8_t>(index)));
    schedule_page_rebuild();
}

void render_stations() {
    reset_live_widgets();
    lv_obj_t *scr = bg_screen();
    button_make(scr, 8, 8, 64, 40, "‹", &mr_font_16, kColorCard, kColorInk,
                kActShowTuner);
    lv_obj_t *title = label_make(scr, "电台列表", &mr_font_24, kColorInk, 0, 8, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    render_tabs(scr, 56);

    uint8_t indices[MR_STATION_COUNT] = {};
    std::size_t count = 0;
    collect_tab_indices(indices, &count);
    const std::size_t pages = count ? (count + kRows - 1) / kRows : 1;
    if (s_list_page >= pages) s_list_page = pages - 1;
    const std::size_t first = s_list_page * kRows;
    const std::size_t last = std::min(first + kRows, count);

    if (count == 0) {
        center_label(label_make(scr, s_list_tab == MR_TAB_FAVORITES
                                            ? "还没有收藏\n在电台行右侧点星标"
                                            : "这一组没有电台",
                                &mr_font_16, kColorMuted, 0, 210, 0), kW);
    }

    for (std::size_t r = 0; r < kRows; ++r) {
        const std::size_t row_abs = first + r;
        if (row_abs >= last) break;
        const uint8_t si = indices[row_abs];
        const bool hot = (si == s_station_index);
        const int y = kListTop + static_cast<int>(r) * kCardPitch;

        lv_obj_t *card = button_make(scr, 12, y, kW - 24, kCardH, "", &mr_font_16,
                                     hot ? kColorCardHot : kColorCard, kColorInk, 0);
        label_make(card, MR_STATIONS[si].name, &mr_font_24, kColorInk, 14, 6, 300);
        char sub[64];
        std::snprintf(sub, sizeof(sub), "%s · %u.%u", mr_cat_name(MR_STATIONS[si].cat),
                      MR_STATIONS[si].freq_decihz / 10, MR_STATIONS[si].freq_decihz % 10);
        label_make(card, sub, &mr_font_16, kColorMuted, 14, 36, 300);

        // 播放中标签（原地更新）。
        lv_obj_t *tag = label_make(card, hot ? "播放中" : "直播", &mr_font_16,
                                   hot ? kColorAccent : kColorMuted, 0, 0, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -64, 0);
        s_row_tag[r] = tag;
        s_row_index[r] = si;

        // 星标按钮。
        const bool fav = mr_is_favorite(s_favorites, si);
        lv_obj_t *star = button_make(card, kW - 24 - 52, 9, 44, 44,
                                     fav ? "★" : "☆", &mr_font_24,
                                     fav ? 0x3A3410 : kColorBgSoft,
                                     fav ? kColorStar : kColorMuted, 0);
        lv_obj_set_style_radius(star, 22, 0);
        s_row_star[r] = star;
        lv_obj_add_event_cb(star, [](lv_event_t *event) {
            star_clicked(lv_event_get_user_data(event));
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::size_t>(si)));

        const std::size_t picked = si;
        lv_obj_add_event_cb(card, [](lv_event_t *event) {
            const std::size_t index =
                reinterpret_cast<std::size_t>(lv_event_get_user_data(event));
            radio_player_set_station(index);
            radio_player_set_playing(true);
            radio_ui_show_tuner();
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(picked));
    }

    const int bottom_y = kH - 64;
    button_make(scr, 12, bottom_y, 140, 52, "上一页", &mr_font_16, kColorCard,
                kColorInk, kActPagePrev);
    char page_text[24];
    std::snprintf(page_text, sizeof(page_text), "%u/%u",
                  static_cast<unsigned>(s_list_page + 1),
                  static_cast<unsigned>(pages));
    center_label(label_make(scr, page_text, &mr_font_16, kColorMuted, 200, bottom_y + 18, 80),
                 80);
    button_make(scr, 248, bottom_y, 140, 52, "下一页", &mr_font_16, kColorCard,
                kColorInk, kActPageNext);
    button_make(scr, 396, bottom_y, 72, 52, "收音", &mr_font_16, kColorAccent,
                kColorBg, kActShowTuner);

    replace_screen(scr);
}

// ---- 设置页 ----

void render_settings() {
    reset_live_widgets();
    lv_obj_t *scr = bg_screen();
    button_make(scr, 8, 8, 64, 40, "‹", &mr_font_16, kColorCard, kColorInk,
                kActShowTuner);
    lv_obj_t *title = label_make(scr, "设置", &mr_font_24, kColorInk, 0, 8, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    char wifi_text[64];
    std::snprintf(wifi_text, sizeof(wifi_text), "WiFi · %s",
                  s_network_up ? (s_network_ssid[0] ? s_network_ssid : "已连接")
                               : "未联网 · 点按配网");
    button_make(scr, 16, 56, kW - 32, 52, wifi_text, &mr_font_16, kColorCard,
                s_network_up ? kColorInk : kColorAccent, kActWifiOpen);

    label_make(scr, "音量", &mr_font_16, kColorMuted, 16, 122, 0);
    button_make(scr, 16, 146, 120, 52, "音量 −", &mr_font_16, kColorCard, kColorInk,
                kActVolDown);
    button_make(scr, 344, 146, 120, 52, "音量 +", &mr_font_16, kColorCard, kColorInk,
                kActVolUp);
    char vol_text[24];
    std::snprintf(vol_text, sizeof(vol_text), "%u", static_cast<unsigned>(s_volume));
    s_vol_label = center_label(label_make(scr, vol_text, &mr_font_24, kColorInk,
                                          148, 158, 0), 184);
    s_vol_bar = bar_make(scr, 16, 208, kW - 32, 8, 100, kColorAccent);
    lv_bar_set_value(s_vol_bar, s_volume, LV_ANIM_OFF);

    label_make(scr, "睡眠定时", &mr_font_16, kColorMuted, 16, 228, 0);
    const char *sleep_names[] = {"关", "15 分", "30 分", "60 分", "90 分"};
    const std::size_t sleep_now = radio_sleep_timer_selection();
    for (int i = 0; i < 5; ++i) {
        const bool on = (static_cast<std::size_t>(i) == sleep_now);
        button_make(scr, 16 + i * 91, 252, 85, 48, sleep_names[i], &mr_font_16,
                    on ? kColorAccent : kColorCard, on ? kColorBg : kColorInk,
                    kActSleepBase + static_cast<uintptr_t>(i));
    }

    char bright_text[40];
    std::snprintf(bright_text, sizeof(bright_text), "亮度 %u%% · 点按切换",
                  static_cast<unsigned>(radio_display_brightness()));
    button_make(scr, 16, 312, kW - 32, 48, bright_text, &mr_font_16, kColorCard,
                kColorInk, kActBrightnessCycle);

    char timeout_text[40];
    std::snprintf(timeout_text, sizeof(timeout_text), "熄屏 %u 秒 · 点按切换",
                  static_cast<unsigned>(radio_display_timeout_seconds()));
    button_make(scr, 16, 368, kW - 32, 48, timeout_text, &mr_font_16, kColorCard,
                kColorInk, kActTimeoutCycle);

    button_make(scr, 16, 424, kW - 32, 48, "回到收音机", &mr_font_16, kColorCardHot,
                kColorInk, kActShowTuner);

    replace_screen(scr);
}

// ---- WiFi 配网三页（english-podcast 真机同款）----

void render_wifi_list() {
    reset_live_widgets();
    lv_obj_t *scr = bg_screen();
    button_make(scr, 8, 8, 64, 40, "‹", &mr_font_16, kColorCard, kColorInk,
                kActWifiCancel);
    lv_obj_t *title = label_make(scr, "选择网络", &mr_font_24, kColorInk, 0, 8, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    if (s_wifi_scanning && s_wifi_count == 0) {
        center_label(label_make(scr, "正在扫描…", &mr_font_16, kColorMuted, 0, 220, 0),
                     kW);
    } else if (s_wifi_count == 0) {
        center_label(label_make(scr, "没有找到网络\n点「重新扫描」再试", &mr_font_16,
                                kColorMuted, 0, 200, 0), kW);
    } else {
        for (std::size_t i = 0; i < s_wifi_count && i < kWifiMaxRows; ++i) {
            const int y = 56 + static_cast<int>(i) * 54;
            lv_obj_t *row = button_make(scr, 16, y, kW - 32, 48, "", &mr_font_16,
                                        kColorCard, kColorInk, 0);
            label_make(row, s_wifi_ssids[i], &mr_font_16, kColorInk, 14, 0, kW - 60);
            const std::size_t index = i;
            lv_obj_add_event_cb(row, [](lv_event_t *event) {
                const std::size_t idx =
                    reinterpret_cast<std::size_t>(lv_event_get_user_data(event));
                if (idx < kWifiMaxRows) {
                    radio_ui_show_wifi_keyboard(s_wifi_ssids[idx]);
                }
            }, LV_EVENT_CLICKED, reinterpret_cast<void *>(index));
        }
    }

    const int bottom_y = kH - 68;
    button_make(scr, 12, bottom_y, 220, 56,
                s_wifi_scanning ? "扫描中…" : "重新扫描", &mr_font_16, kColorCard,
                kColorInk, kActWifiRescan);
    button_make(scr, 248, bottom_y, 220, 56, "取消", &mr_font_16, kColorCard,
                kColorInk, kActWifiCancel);
    replace_screen(scr);
}

void wifi_key_append(char c) {
    const std::size_t len = std::strlen(s_wifi_pass);
    if (len + 1 >= sizeof(s_wifi_pass)) return;
    s_wifi_pass[len] = c;
    s_wifi_pass[len + 1] = '\0';
    schedule_page_rebuild();
}

void wifi_key_backspace() {
    const std::size_t len = std::strlen(s_wifi_pass);
    if (len > 0) s_wifi_pass[len - 1] = '\0';
    schedule_page_rebuild();
}

void render_wifi_keyboard() {
    reset_live_widgets();
    lv_obj_t *scr = bg_screen();
    button_make(scr, 8, 8, 64, 40, "‹", &mr_font_16, kColorCard, kColorInk,
                kActWifiBack);
    lv_obj_t *title = label_make(scr, "输入密码", &mr_font_24, kColorInk, 0, 8, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    label_make(scr, s_wifi_target, &mr_font_16, kColorMuted, 16, 52, kW - 32);
    char masked[70];
    const std::size_t len = std::strlen(s_wifi_pass);
    std::memset(masked, '*', len);
    masked[len] = '\0';
    label_make(scr, masked, &mr_font_24, kColorInk, 16, 78, kW - 32);

    // 键盘 4 行；行内字符同时是字模来源。
    const char *rows = nullptr;
    switch (s_wifi_key_mode) {
        case 0: rows = "1234567890\0qwertyuiop\0asdfghjkl\0zxcvbnm"; break;
        case 1: rows = "1234567890\0QWERTYUIOP\0ASDFGHJKL\0ZXCVBNM"; break;
        default: rows = "!@#$%^&*()\0-_=+[]{};\0,.<>/?|~`\0\"':\\"; break;
    }
    constexpr int kKeyW = 44;
    constexpr int kKeyPitch = 45;
    constexpr int kKeyH = 42;
    constexpr int kKeyTop = 118;
    constexpr int kKeyPitchY = 46;
    for (int r = 0; r < 4; ++r) {
        const char *row = rows;
        for (int k = 0; k < r; ++k) row += std::strlen(row) + 1;
        const std::size_t cols = std::strlen(row);
        for (std::size_t c = 0; c < cols && c < 10; ++c) {
            char key_text[2] = {row[c], '\0'};
            lv_obj_t *key = button_make(scr, 12 + static_cast<int>(c) * kKeyPitch,
                                        kKeyTop + r * kKeyPitchY, kKeyW, kKeyH,
                                        key_text, &mr_font_16, kColorCard, kColorInk,
                                        0);
            const char ch = row[c];
            lv_obj_add_event_cb(key, [](lv_event_t *event) {
                const uintptr_t ch =
                    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
                wifi_key_append(static_cast<char>(ch));
            }, LV_EVENT_CLICKED, reinterpret_cast<void *>(ch));
        }
    }

    constexpr int kBottomY = kKeyTop + 4 * kKeyPitchY + 2;
    button_make(scr, 12, kBottomY, 170, 46, "空格", &mr_font_16, kColorCard,
                kColorInk, kActWifiSpace);
    button_make(scr, 190, kBottomY, 90, 46, "退格", &mr_font_16, kColorCard,
                kColorInk, kActWifiBackspace);
    const char *mode_name = s_wifi_key_mode == 0 ? "ABC"
                            : s_wifi_key_mode == 1 ? "abc"
                                                   : "符号";
    button_make(scr, 288, kBottomY, 90, 46, mode_name, &mr_font_16, kColorCard,
                kColorInk, kActWifiKeyMode);
    button_make(scr, 386, kBottomY, 82, 46, "连接", &mr_font_16, kColorAccent,
                kColorBg, kActWifiConnect);

    char hint[48];
    std::snprintf(hint, sizeof(hint), "连接 %s", s_wifi_target);
    label_make(scr, hint, &mr_font_16, kColorMuted, 16, kH - 24, 0);
    replace_screen(scr);
}

void render_wifi_connecting() {
    reset_live_widgets();
    lv_obj_t *scr = bg_screen();
    lv_obj_t *title = label_make(scr, "配置网络", &mr_font_24, kColorInk, 0, 8, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    center_label(label_make(scr, s_wifi_target, &mr_font_24, kColorInk, 0, 150, 0),
                 kW);
    s_wifi_status_label =
        center_label(label_make(scr, "正在连接…", &mr_font_16, kColorMuted, 0, 200, 0),
                     kW);
    center_label(label_make(scr, "配置成功后设备将自动重启", &mr_font_16,
                            kColorMuted, 0, 240, 0), kW);

    button_make(scr, 130, 320, 220, 56, "返回", &mr_font_16, kColorCard,
                kColorInk, kActWifiBack);
    replace_screen(scr);
}

// ---- 整页重建统一走异步 ----

bool s_rebuild_pending = false;

void do_page_rebuild() {
    hide_volume_overlay();
    switch (s_page) {
        case RadioPage::Tuner:
            render_tuner();
            break;
        case RadioPage::Stations:
            render_stations();
            break;
        case RadioPage::Settings:
            render_settings();
            break;
        case RadioPage::WifiList:
            render_wifi_list();
            break;
        case RadioPage::WifiKeyboard:
            render_wifi_keyboard();
            break;
        case RadioPage::WifiConnecting:
            render_wifi_connecting();
            break;
    }
}

void rebuild_async(void *) {
    s_rebuild_pending = false;
    do_page_rebuild();
}

void schedule_page_rebuild() {
    if (!bsp_display_lock(500)) {
        ESP_LOGW(kTag, "cannot lock LVGL for rebuild");
        return;
    }
    if (!s_rebuild_pending) {
        s_rebuild_pending = true;
        lv_async_call(rebuild_async, nullptr);
    }
    bsp_display_unlock();
}

// ---- 原地更新 ----

void hide_volume_overlay() {
    if (s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    // 弹层停显就删掉自动隐藏定时器（否则它每 1.6s 空转一次）。
    if (s_overlay_timer) {
        lv_timer_del(s_overlay_timer);
        s_overlay_timer = nullptr;
    }
}

void overlay_expire(lv_timer_t *timer) {
    (void)timer;
    if (!bsp_display_lock(200)) return;
    hide_volume_overlay();
    bsp_display_unlock();
}

void volume_overlay_kick() {
    if (!s_overlay) return;
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    char vol_text[16];
    std::snprintf(vol_text, sizeof(vol_text), "%u", static_cast<unsigned>(s_volume));
    lv_label_set_text(s_overlay_label, vol_text);
    lv_bar_set_value(s_overlay_bar, s_volume, LV_ANIM_OFF);
    if (s_overlay_timer) {
        lv_timer_reset(s_overlay_timer);
    } else {
        s_overlay_timer = lv_timer_create(overlay_expire, 1600, nullptr);
    }
}

bool update_tuner_widgets() {
    // 状态行 + 时钟 + 网络徽标。
    if (s_status_label) {
        char line[96];
        if (s_playing && s_network_up) {
            char mmss[16];
            fmt_mmss(mmss, sizeof(mmss), radio_player_listen_ms());
            const uint32_t hz = radio_player_stream_hz();
            if (hz) {
                std::snprintf(line, sizeof(line), "%s · %u.%u kHz · 收听 %s",
                              playback_state_text(),
                              static_cast<unsigned>(hz / 1000),
                              static_cast<unsigned>((hz % 1000) / 100),
                              mmss);
            } else {
                std::snprintf(line, sizeof(line), "%s · 收听 %s",
                              playback_state_text(), mmss);
            }
        } else {
            std::snprintf(line, sizeof(line), "%s", playback_state_text());
        }
        lv_label_set_text(s_status_label, line);
    }
    if (s_clock_label) {
        char hhmm[8];
        if (radio_clock_hhmm(hhmm, sizeof(hhmm))) {
            lv_label_set_text(s_clock_label, hhmm);
        }
    }
    // VU：播放中拉实时值（快攻慢释）；停播按 78%/tick 衰减到 0。
    uint8_t live[MR_LEVEL_COUNT] = {};
    if (s_playing) {
        radio_player_levels(live, MR_LEVEL_COUNT);
    }
    for (int i = 0; i < MR_LEVEL_COUNT; ++i) {
        const uint8_t target = live[i];
        uint8_t shown = s_vu_display[i];
        if (s_playing) {
            shown = target > shown ? target
                                   : static_cast<uint8_t>((shown * 2 + target) / 3);
        } else {
            shown = static_cast<uint8_t>(shown * 78 / 100);
        }
        s_vu_display[i] = shown;
        if (s_vu_bar[i]) lv_bar_set_value(s_vu_bar[i], shown, LV_ANIM_OFF);
    }
    return true;
}

bool update_station_widgets() {
    bool any_changed = false;
    const uint8_t current = static_cast<uint8_t>(radio_player_station_index());
    for (int r = 0; r < static_cast<int>(kRows); ++r) {
        if (!s_row_tag[r]) continue;
        const bool hot = (s_row_index[r] == current);
        const char *text = hot ? "播放中" : "直播";
        lv_label_set_text(s_row_tag[r], text);
        lv_obj_set_style_text_color(
            s_row_tag[r], lv_color_hex(hot ? kColorAccent : kColorMuted), 0);
        any_changed = true;
    }
    return any_changed;
}

// 200ms 同步定时器：Tuner 页推进 VU/状态/时钟；列表页刷新「播放中」标记。
void sync_tick(lv_timer_t *) {
    if (s_page == RadioPage::Tuner) {
        if (!bsp_display_lock(100)) return;
        update_tuner_widgets();
        bsp_display_unlock();
    } else if (s_page == RadioPage::Stations) {
        if (!bsp_display_lock(100)) return;
        update_station_widgets();
        bsp_display_unlock();
    }
}

}  // namespace

// ---- 动作分发 ----

void radio_ui_handle_action(uintptr_t action) {
    switch (action) {
        case kActShowTuner:
            radio_ui_show_tuner();
            break;
        case kActShowStations:
            radio_ui_show_stations();
            break;
        case kActShowSettings:
            radio_ui_show_settings();
            break;
        case kActStationPrev:
        case kActDialPrev:
            radio_player_select_relative(-1);
            schedule_page_rebuild();
            break;
        case kActStationNext:
        case kActDialNext:
            radio_player_select_relative(1);
            schedule_page_rebuild();
            break;
        case kActPlayToggle:
            radio_player_toggle();
            schedule_page_rebuild();
            break;
        case kActVolOpen:
            // 弹层只在 Tuner 页有效；其他页 +/− 直接改。
            if (s_overlay) volume_overlay_kick();
            break;
        case kActVolDown:
            // radio_player_set_volume → radio_ui_set_volume 已经原地更新了
            // 主页弹层与设置页音量条，这里不再整页重建（重建会把弹层藏掉）。
            radio_player_set_volume(radio_player_volume() > 20
                                        ? radio_player_volume() - 10
                                        : 10);
            volume_overlay_kick();
            break;
        case kActVolUp:
            radio_player_set_volume(radio_player_volume() + 10);
            volume_overlay_kick();
            break;
        case kActPagePrev:
            if (s_list_page > 0) {
                --s_list_page;
                schedule_page_rebuild();
            }
            break;
        case kActPageNext: {
            uint8_t indices[MR_STATION_COUNT] = {};
            std::size_t count = 0;
            collect_tab_indices(indices, &count);
            const std::size_t pages = count ? (count + kRows - 1) / kRows : 1;
            if (s_list_page + 1 < pages) {
                ++s_list_page;
                schedule_page_rebuild();
            }
            break;
        }
        case kActBrightnessCycle:
            radio_display_set_brightness_selection(
                radio_display_brightness_selection() + 1);
            schedule_page_rebuild();
            break;
        case kActTimeoutCycle:
            radio_display_set_timeout_selection(
                radio_display_timeout_selection() + 1);
            schedule_page_rebuild();
            break;
        // ---- WiFi 配网 ----
        case kActWifiOpen:
            s_page = RadioPage::WifiList;
            schedule_page_rebuild();
            radio_wifi_request_scan();
            break;
        case kActWifiRescan:
            radio_wifi_request_scan();
            break;
        case kActWifiCancel:
            s_page = RadioPage::Settings;
            schedule_page_rebuild();
            radio_wifi_request_cancel();
            break;
        case kActWifiBack:
            s_page = RadioPage::WifiList;
            schedule_page_rebuild();
            break;
        case kActWifiConnect:
            if (s_wifi_pass[0]) {
                s_page = RadioPage::WifiConnecting;
                schedule_page_rebuild();
                radio_wifi_request_connect(s_wifi_target, s_wifi_pass);
            }
            break;
        case kActWifiKeyMode:
            s_wifi_key_mode = (s_wifi_key_mode + 1) % 3;
            schedule_page_rebuild();
            break;
        case kActWifiSpace:
            wifi_key_append(' ');
            break;
        case kActWifiBackspace:
            wifi_key_backspace();
            break;
        default:
            // 分组 tab / 睡眠定时：kActTabBase + i / kActSleepBase + i。
            // MR_TAB_COUNT 是 C 侧 enum，与动作枚举不能直接混算（-Werror）。
            if (action >= kActTabBase &&
                action < kActTabBase + static_cast<uintptr_t>(MR_TAB_COUNT)) {
                const uint8_t tab = static_cast<uint8_t>(action - kActTabBase);
                if (s_page == RadioPage::Stations) {
                    s_list_tab = tab;
                    s_list_page = 0;
                } else if (tab == MR_TAB_FAVORITES) {
                    uint8_t first_fav = 0;
                    if (mr_favorite_at(s_favorites, 0, &first_fav)) {
                        radio_player_set_station(first_fav);
                    } else {
                        // 没有收藏：去列表页，让用户挑一个。
                        s_list_tab = tab;
                        s_list_page = 0;
                        s_page = RadioPage::Stations;
                    }
                } else {
                    // 跳到该组第一台。
                    for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
                        if (MR_STATIONS[i].cat == tab) {
                            radio_player_set_station(i);
                            break;
                        }
                    }
                }
                schedule_page_rebuild();
            } else if (action >= kActSleepBase &&
                       action < kActSleepBase + 5) {
                static const uint16_t kMinutes[] = {0, 15, 30, 60, 90};
                radio_sleep_timer_set_minutes(
                    kMinutes[action - kActSleepBase]);
                schedule_page_rebuild();
            }
            break;
    }
}

bool radio_ui_init() {
    if (!bsp_display_lock(-1)) return false;
    s_page = RadioPage::Tuner;
    if (!s_screen) {
        do_page_rebuild();
        s_sync_timer = lv_timer_create(sync_tick, 200, nullptr);
    }
    bsp_display_unlock();
    return true;
}

void radio_ui_show_tuner() {
    s_page = RadioPage::Tuner;
    schedule_page_rebuild();
}

void radio_ui_show_stations() {
    // 列表页落在本组：从 tuner 的分组 chip 跳过来时语义一致。
    s_list_tab = MR_STATIONS[s_station_index].cat;
    s_list_page = 0;
    s_page = RadioPage::Stations;
    schedule_page_rebuild();
}

void radio_ui_show_settings() {
    s_page = RadioPage::Settings;
    schedule_page_rebuild();
}

RadioPage radio_ui_current_page() {
    return s_page;
}

void radio_ui_show_wifi_list() {
    s_page = RadioPage::WifiList;
    schedule_page_rebuild();
}

void radio_ui_show_wifi_keyboard(const char *ssid) {
    std::snprintf(s_wifi_target, sizeof(s_wifi_target), "%s", ssid ? ssid : "");
    s_wifi_pass[0] = '\0';
    s_wifi_key_mode = 0;
    s_page = RadioPage::WifiKeyboard;
    schedule_page_rebuild();
}

void radio_ui_show_wifi_connecting(const char *ssid) {
    std::snprintf(s_wifi_target, sizeof(s_wifi_target), "%s", ssid ? ssid : "");
    s_page = RadioPage::WifiConnecting;
    schedule_page_rebuild();
}

void radio_ui_set_wifi_scan_results(const char (*ssids)[33], std::size_t count,
                                    bool scanning) {
    s_wifi_count = std::min(count, kWifiMaxRows);
    for (std::size_t i = 0; i < s_wifi_count; ++i) {
        std::snprintf(s_wifi_ssids[i], sizeof(s_wifi_ssids[i]), "%s", ssids[i]);
    }
    s_wifi_scanning = scanning;
    if (s_page == RadioPage::WifiList) schedule_page_rebuild();
}

void radio_ui_set_wifi_connect_text(const char *text) {
    if (s_wifi_status_label && text) {
        if (!bsp_display_lock(200)) return;
        lv_label_set_text(s_wifi_status_label, text);
        bsp_display_unlock();
    }
}

// 播放器任务的状态回调：尽量原地改，改不动才重建。
void refresh_current_page_if_visible() {
    switch (s_page) {
        case RadioPage::Tuner:
            if (!bsp_display_lock(500)) {
                schedule_page_rebuild();
                return;
            }
            update_tuner_widgets();
            bsp_display_unlock();
            return;
        case RadioPage::Stations:
            if (!bsp_display_lock(500)) {
                schedule_page_rebuild();
                return;
            }
            update_station_widgets();
            bsp_display_unlock();
            return;
        case RadioPage::Settings:
        case RadioPage::WifiList:
        case RadioPage::WifiKeyboard:
        case RadioPage::WifiConnecting:
            return;
    }
}

void radio_ui_set_station(std::size_t index, std::size_t count,
                          const mr_station_snapshot_t &station) {
    (void)count;
    s_station_index = index;
    std::snprintf(s_station_name, sizeof(s_station_name), "%s", station.name);
    s_station_freq = station.freq_decihz;
    s_station_cat = station.cat;
    if (bsp_display_lock(200)) {
        if (s_name_label) {
            lv_label_set_text(s_name_label, s_station_name);
        }
        if (s_meta_label) {
            char meta[64];
            std::snprintf(meta, sizeof(meta), "%s · 调谐 %u.%u",
                          mr_cat_name(s_station_cat),
                          s_station_freq / 10, s_station_freq % 10);
            lv_label_set_text(s_meta_label, meta);
            lv_obj_set_style_text_color(
                s_meta_label, lv_color_hex(kCatColor[s_station_cat]), 0);
        }
        bsp_display_unlock();
    }
    refresh_current_page_if_visible();
}

void radio_ui_set_playback(RadioPlaybackState state, const char *detail) {
    s_playing = (state == RadioPlaybackState::Playing);
    if (detail && detail[0]) {
        std::snprintf(s_playback_text, sizeof(s_playback_text), "%.*s",
                      static_cast<int>(sizeof(s_playback_text)) - 1, detail);
    }
    refresh_current_page_if_visible();
}

void radio_ui_set_volume(uint8_t volume) {
    s_volume = volume;
    if (!bsp_display_lock(200)) return;
    if (s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        volume_overlay_kick();
    }
    if (s_vol_label) {
        char vol_text[24];
        std::snprintf(vol_text, sizeof(vol_text), "%u", static_cast<unsigned>(s_volume));
        lv_label_set_text(s_vol_label, vol_text);
    }
    if (s_vol_bar) lv_bar_set_value(s_vol_bar, s_volume, LV_ANIM_OFF);
    bsp_display_unlock();
}

void radio_ui_set_favorites(uint32_t favorites) {
    s_favorites = favorites;
    if (s_page == RadioPage::Stations) schedule_page_rebuild();
}

void radio_ui_set_network(bool connected, const char *ssid) {
    s_network_up = connected;
    std::snprintf(s_network_ssid, sizeof(s_network_ssid), "%s", ssid ? ssid : "");
    if (s_page == RadioPage::Tuner || s_page == RadioPage::Settings ||
        s_page == RadioPage::Stations) {
        schedule_page_rebuild();
    }
}
