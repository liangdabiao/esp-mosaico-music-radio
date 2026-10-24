// main/radio_player.cc —— 直播流播放器（蜻蜓 FM 音乐台，连续流）。
//
// 管线血统来自 english-podcast 的分段点播器（HTTP → 环形缓冲 →
// esp_audio_simple_dec MP3 → ES8311），按直播改造：
//
//   1. 单 URL 无限流：没有段、没有 Range 断点、没有「播放完成」。
//      net_task 只管把流往前拉（读断了自带退避重试），播放端只管消费；
//      环空且网络侧已死 → 会话结束，播放任务择机重连。
//   2. 预取环加大到 48 KB（64 kbps ≈ 8 KB/s，约 6 秒余量），存 PSRAM。
//      直播流永远是一条 24×7 的长连接，抖动容忍比点播更重要。
//   3. 素材是 44.1 kHz joint-stereo 音乐：等权 downmix（(L+R)/2，不削波），
//      音量曲线按音乐响度重标定（{0:-44 dB, 100:+4 dB} —— 语料用的
//      -40..+12 曲线对满量程音乐会把峰值顶进削波区）。
//   4. VU 能级条在解码线程持续刷新（拉模型，UI 轮询），换台即清零。
#include "radio_player.h"

#include "bsp/audio.h"
#include "decoder/esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#include "esp_attr.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio_ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "radio_player";
constexpr std::size_t kInputSize = 1024;
constexpr std::size_t kOutputSize = 8192;

// ---- 预取环（PSRAM）----
// 64 kbps ≈ 8 KB/s：48 KB ≈ 6 秒。直播不断流，但 Wi-Fi 侧偶发 100-500 ms
// 停顿；6 秒余量配合 2.5 秒重连预算，听感上「不断」。
constexpr std::size_t kRingBytes = 48 * 1024;
constexpr std::size_t kPrebufferBytes = 16384;   // ≈ 2 秒再开播
constexpr uint32_t kPrebufferWaitMs = 4000;

constexpr TickType_t kRingRecvTimeout = pdMS_TO_TICKS(100);
// 必须带超时：缓冲满时死等会让 net_task 在换台后永远卡在写入上。
constexpr TickType_t kRingSendTimeout = pdMS_TO_TICKS(200);

constexpr uint32_t kHttpTimeoutMs = 5000;
constexpr int kStreamRetries = 5;                 // net_task 内部重试轮数
constexpr uint32_t kRetryStepMs = 600;
// 缓冲空了多久放弃本会话（net_task 还活着时不轻易放弃）。
constexpr uint32_t kStarveGiveUpMs = 12000;
constexpr uint32_t kStarveLogEveryMs = 3000;

// 音频产出看门狗（同 english-podcast）：有数据喂但喇叭 N 秒无声 → 重建通路。
constexpr uint32_t kSilenceRestartMs = 5000;
constexpr uint8_t kMaxAudioRestarts = 2;
constexpr uint32_t kHealthLogEveryMs = 10000;
constexpr uint32_t kNetTaskStack = 8192;

// 播放任务对同一台的连续失败容忍；超过就换下一台（死台自救）。
constexpr uint8_t kMaxFailedAttempts = 4;

// 预取环本体放 PSRAM（CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y）。
static EXT_RAM_BSS_ATTR uint8_t s_ring_storage[kRingBytes];
static StaticStreamBuffer_t s_ring_struct;

std::atomic<bool> s_network_connected{false};
std::atomic<bool> s_wanted_playing{false};
std::atomic<std::size_t> s_station_index{0};
std::atomic<uint8_t> s_volume{80};
std::atomic<uint32_t> s_generation{1};
std::atomic<bool> s_stream_active{false};
std::atomic<uint32_t> s_stream_hz{0};
TaskHandle_t s_player_task;
SemaphoreHandle_t s_station_mutex;

// 静态电台表是常量，直接读没有并发问题；锁只保护「当前台」这类可变镜像。
std::atomic<uint32_t> s_favorites{0};

// ---- 预取任务共享 ----
StreamBufferHandle_t s_ring = nullptr;
std::atomic<bool> s_net_active{false};
std::atomic<bool> s_net_done{false};    // 网络侧已结束（重试耗尽或被取消）
std::atomic<bool> s_net_failed{false};  // 结束原因是失败（而不是会话被换）
std::atomic<std::size_t> s_session_station{0};
std::atomic<uint32_t> s_session_generation{0};

// 收听时长（按已解码的单声道样本推进）。
std::atomic<uint64_t> s_listen_samples{0};

// ---- VU ----
std::atomic<uint64_t> s_level_epoch{1};   // 换台/停播时递增，UI 据此清零衰减
std::atomic<uint8_t> s_levels[MR_LEVEL_COUNT] = {};

// ---- 音频通路（ES8311 via esp_codec_dev）----
esp_codec_dev_handle_t s_codec = nullptr;
uint32_t s_codec_rate = 0;
bool s_codec_open = false;

bool audio_apply_volume() {
    if (!s_codec || !s_codec_open) return false;
    return esp_codec_dev_set_out_vol(s_codec, s_volume.load()) == ESP_CODEC_DEV_OK;
}

// 音量曲线按音乐素材重标定。64 kbps 广播流的峰值普遍贴近满量程（实测
// 动感101 段落 RMS ≈ -16 dBFS、峰值 -1 dBFS），english-podcast 给语料用的
// -40..+12 dB 曲线在这里会把 80 档顶到 +1.6 dB、峰值削波。改用 -44..+4：
// 80 档 ≈ -5.5 dB（大声而不破），想更响仍有 100 档。
// 必须按 vol 升序：_get_vol_db 用「vol >= 末项」提前返回，降序写会全错。
void install_volume_curve(esp_codec_dev_handle_t codec) {
    if (!codec) return;
    static esp_codec_dev_vol_map_t s_vol_map[2] = {
        {0, -44.0f},
        {100, 4.0f},
    };
    esp_codec_dev_vol_curve_t curve = {};
    curve.vol_map = s_vol_map;
    curve.count = 2;
    if (esp_codec_dev_set_vol_curve(codec, &curve) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "volume curve rejected; on default -50..0 dB");
    }
}

bool audio_open_at(uint32_t rate_hz) {
    if (!s_codec) return false;
    if (s_codec_open && s_codec_rate == rate_hz) return true;
    if (s_codec_open) esp_codec_dev_close(s_codec);
    esp_codec_dev_sample_info_t info = {};
    info.sample_rate = static_cast<int>(rate_hz);
    info.channel = 1;
    info.bits_per_sample = 16;
    if (esp_codec_dev_open(s_codec, &info) != ESP_CODEC_DEV_OK) {
        s_codec_open = false;
        return false;
    }
    s_codec_open = true;
    s_codec_rate = rate_hz;
    // close/open 清驱动状态，音量曲线必须重挂（english-podcast 实测坑）。
    install_volume_curve(s_codec);
    audio_apply_volume();
    return true;
}

void audio_close() {
    if (s_codec && s_codec_open) esp_codec_dev_close(s_codec);
    s_codec_open = false;
    s_codec_rate = 0;
}

bool audio_write(const void *pcm, std::size_t bytes) {
    if (!s_codec || !s_codec_open) return false;
    return esp_codec_dev_write(s_codec, const_cast<void *>(pcm),
                               static_cast<int>(bytes)) == ESP_CODEC_DEV_OK;
}

mr_station_snapshot_t station_snapshot(std::size_t index) {
    mr_station_snapshot_t snap = {};
    if (index >= MR_STATION_COUNT) return snap;
    const mr_station_t &st = MR_STATIONS[index];
    std::snprintf(snap.name, sizeof(snap.name), "%s", st.name);
    std::snprintf(snap.url, sizeof(snap.url), "%s", st.url);
    snap.cat = st.cat;
    snap.freq_decihz = st.freq_decihz;
    return snap;
}

uint32_t now_ms() {
    return static_cast<uint32_t>(xTaskGetTickCount()) *
           static_cast<uint32_t>(portTICK_PERIOD_MS);
}

bool request_still_current(uint32_t generation, std::size_t station) {
    return s_network_connected.load(std::memory_order_acquire) &&
           s_wanted_playing.load(std::memory_order_acquire) &&
           s_generation.load(std::memory_order_acquire) == generation &&
           s_station_index.load(std::memory_order_acquire) == station;
}

// ---- NVS ----

uint8_t load_u8(const char *key, uint8_t fallback) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

void save_u8(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

uint32_t load_u32(const char *key, uint32_t fallback) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint32_t value = fallback;
    if (nvs_get_u32(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

void save_u32(const char *key, uint32_t value) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

void persist_station(uint8_t index) {
    save_u8("station", index);
    // 台名是第二重保险：清单版本变动后按名字找回上次收听的台。
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    if (index < MR_STATION_COUNT) {
        nvs_set_str(handle, "station_name", MR_STATIONS[index].name);
    }
    nvs_commit(handle);
    nvs_close(handle);
}

// 清单升级后索引可能变，恢复时优先按台名对回（对不上再用索引）。
uint8_t restore_station_index() {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) == ESP_OK) {
        char name[48] = {};
        std::size_t len = sizeof(name);
        const esp_err_t err = nvs_get_str(handle, "station_name", name, &len);
        nvs_close(handle);
        if (err == ESP_OK && name[0]) {
            for (uint8_t i = 0; i < MR_STATION_COUNT; ++i) {
                if (std::strcmp(MR_STATIONS[i].name, name) == 0) return i;
            }
        }
    }
    return load_u8("station", 0) % MR_STATION_COUNT;
}

// ---- VU 能级 ----
// 每 18 根条对应一段 PCM 帧；RMS→dB 后铺到 0-100。轻加一点点抖动让
// 静音段不至于「全条躺平」（真机的观感比精确更重要）。
void calculate_levels(const uint8_t *pcm, std::size_t bytes) {
    if (!pcm || bytes < 64) return;
    const auto *samples = reinterpret_cast<const int16_t *>(pcm);
    const std::size_t frame_count = bytes / sizeof(int16_t);
    if (frame_count < MR_LEVEL_COUNT) return;

    uint8_t next[MR_LEVEL_COUNT] = {};
    for (int band = 0; band < MR_LEVEL_COUNT; ++band) {
        const std::size_t begin = band * frame_count / MR_LEVEL_COUNT;
        const std::size_t end = (band + 1) * frame_count / MR_LEVEL_COUNT;
        int64_t energy = 0;
        std::size_t count = 0;
        for (std::size_t i = begin; i < end; ++i) {
            energy += static_cast<int64_t>(samples[i]) * samples[i];
            ++count;
        }
        const float rms = count ? std::sqrt(static_cast<float>(energy) / count) : 0.0f;
        const float db = rms > 1.0f ? 20.0f * std::log10(rms / 32768.0f) : -80.0f;
        int value = static_cast<int>((db + 52.0f) * 1.7f);
        value += static_cast<int>((band * 13 + s_level_epoch.load()) % 7) - 3;
        next[band] = static_cast<uint8_t>(std::clamp(value, 2, 100));
    }
    for (int band = 0; band < MR_LEVEL_COUNT; ++band) {
        s_levels[band].store(next[band], std::memory_order_relaxed);
    }
}

void clear_levels() {
    s_level_epoch.fetch_add(1, std::memory_order_release);
    for (int band = 0; band < MR_LEVEL_COUNT; ++band) {
        s_levels[band].store(0, std::memory_order_relaxed);
    }
}

std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~static_cast<std::size_t>(1);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    // 等权平均：(L+R)/2。峰值不会越过单声道满量程，无需限幅器。
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        samples[frame] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

// ---- 取流：HTTP → 环形缓冲 ----

bool push_body_into_ring(esp_http_client_handle_t client, std::size_t station,
                         uint32_t generation) {
    uint8_t chunk[2048];
    while (true) {
        if (!request_still_current(generation, station)) return false;
        const int received =
            esp_http_client_read(client, reinterpret_cast<char *>(chunk), sizeof(chunk));
        if (received < 0) return false;   // 底层错误 → 重试
        if (received == 0) return true;   // 连接被服务端关闭 → 重试
        std::size_t sent = 0;
        const std::size_t total = static_cast<std::size_t>(received);
        while (sent < total) {
            const std::size_t n = xStreamBufferSend(s_ring, chunk + sent, total - sent,
                                                    kRingSendTimeout);
            if (n > 0) {
                sent += n;
                continue;
            }
            if (!request_still_current(generation, station)) return false;
        }
    }
}

void net_task(void *) {
    // 会话快照：play_stream 创建任务前写入，任务启动后立即读走 —— 之后
    // 用户换台不会影响这一轮已经在跑的取流。
    const std::size_t station = s_session_station.load(std::memory_order_acquire);
    const uint32_t generation = s_session_generation.load(std::memory_order_acquire);
    const mr_station_snapshot_t snap = station_snapshot(station);

    bool session_cancelled = false;
    if (!snap.url[0]) {
        ESP_LOGE(kTag, "station %u has no url", static_cast<unsigned>(station));
        s_net_failed.store(true, std::memory_order_release);
        s_net_done.store(true, std::memory_order_release);
        s_net_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    for (int attempt = 0; attempt < kStreamRetries; ++attempt) {
        if (!request_still_current(generation, station)) {
            session_cancelled = true;
            break;
        }
        if (attempt > 0) {
            ESP_LOGW(kTag, "stream reconnect attempt %d/%d for %s",
                     attempt + 1, kStreamRetries, snap.name);
            const uint32_t backoff = kRetryStepMs * (attempt + 1);
            for (uint32_t waited = 0; waited < backoff; waited += 100) {
                if (!request_still_current(generation, station)) break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!request_still_current(generation, station)) {
                session_cancelled = true;
                break;
            }
        }

        esp_http_client_config_t config = {};
        config.url = snap.url;
        config.timeout_ms = kHttpTimeoutMs;
        config.buffer_size = 2048;
        config.buffer_size_tx = 512;
        config.user_agent = "MosaicoMusicRadio/1.0";
        config.keep_alive_enable = true;
        config.disable_auto_redirect = false;
        config.max_redirection_count = 4;
        // 平台若把 http 强制 301 到 https，客户端会跟着跳，校验靠证书包；
        // 明文连接下挂着无害。
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(kTag, "HTTP client allocation failed");
            continue;   // 多半堆瞬时不够，值得下一轮再试
        }
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");

        esp_err_t open_err = esp_http_client_open(client, 0);
        if (open_err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            const int status = esp_http_client_get_status_code(client);
            if (status >= 200 && status < 300) {
                if (!push_body_into_ring(client, station, generation)) {
                    // 会话被取消；真网络错误留到下一轮重试。
                    if (!request_still_current(generation, station)) {
                        esp_http_client_close(client);
                        esp_http_client_cleanup(client);
                        session_cancelled = true;
                        break;
                    }
                }
            } else {
                ESP_LOGW(kTag, "HTTP %d for %s", status, snap.url);
            }
        } else {
            ESP_LOGW(kTag, "open failed: %s", snap.url);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    s_net_failed.store(!session_cancelled, std::memory_order_release);
    s_net_done.store(true, std::memory_order_release);
    s_net_active.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void wait_net_idle() {
    const uint32_t deadline = now_ms() + kHttpTimeoutMs + 1500;
    while (s_net_active.load(std::memory_order_acquire) && now_ms() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================ 播放侧

struct StreamState {
    esp_audio_simple_dec_handle_t dec = nullptr;
    bool dec_ready = false;
    uint8_t *input = nullptr;
    uint8_t *output = nullptr;
    bool format_ready = false;
    uint8_t source_channels = 1;
    bool played_audio = false;
};

bool open_pipeline(StreamState *state) {
    state->input = static_cast<uint8_t *>(malloc(kInputSize));
    state->output = static_cast<uint8_t *>(malloc(kOutputSize));
    if (!state->input || !state->output) {
        ESP_LOGE(kTag, "not enough memory for stream buffers");
        std::free(state->output);
        std::free(state->input);
        state->input = nullptr;
        state->output = nullptr;
        return false;
    }
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    cfg.use_frame_dec = false;
    esp_audio_simple_dec_open(&cfg, &state->dec);
    if (!state->dec) {
        ESP_LOGE(kTag, "cannot open MP3 decoder");
        std::free(state->output);
        std::free(state->input);
        state->input = nullptr;
        state->output = nullptr;
        return false;
    }
    state->dec_ready = true;
    return true;
}

void close_pipeline(StreamState *state) {
    if (state->dec_ready && state->dec) {
        esp_audio_simple_dec_close(state->dec);
    }
    state->dec = nullptr;
    state->dec_ready = false;
    std::free(state->output);
    std::free(state->input);
    state->input = nullptr;
    state->output = nullptr;
}

bool restart_audio(StreamState *state) {
    close_pipeline(state);
    audio_close();
    if (!open_pipeline(state)) return false;
    state->format_ready = false;
    return true;
}

enum class StreamOutcome { Failed, Dropped, NetLost };

// 一次会话：起 net_task → 预取 → 解码循环。直播没有「完成」：
// 环空且网络侧失败 = NetLost（播放任务重连），会话被换 = Dropped。
StreamOutcome play_stream(std::size_t station, uint32_t generation) {
    const mr_station_snapshot_t snap = station_snapshot(station);
    if (!snap.url[0]) return StreamOutcome::Failed;

    StreamState state;
    if (!open_pipeline(&state)) return StreamOutcome::Failed;

    clear_levels();
    s_listen_samples.store(0, std::memory_order_release);

    bool played_anything = false;
    uint32_t last_audio_ms = now_ms();
    uint32_t health_logged_ms = last_audio_ms;
    uint32_t write_failures = 0;
    uint32_t audio_restarts = 0;
    uint32_t decoded_bytes_total = 0;

    xStreamBufferReset(s_ring);
    s_net_done.store(false, std::memory_order_release);
    s_net_failed.store(false, std::memory_order_release);
    s_net_active.store(true, std::memory_order_release);
    s_session_station.store(station, std::memory_order_release);
    s_session_generation.store(generation, std::memory_order_release);
    if (xTaskCreate(net_task, "mr_net", kNetTaskStack, nullptr, 5, nullptr) != pdPASS) {
        s_net_active.store(false, std::memory_order_release);
        ESP_LOGE(kTag, "cannot create net task");
        close_pipeline(&state);
        return StreamOutcome::Failed;
    }

    radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
    uint32_t wait_ms = 0;
    while (xStreamBufferBytesAvailable(s_ring) < kPrebufferBytes &&
           wait_ms < kPrebufferWaitMs && !s_net_done.load(std::memory_order_acquire) &&
           request_still_current(generation, station)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }

    uint32_t starve_since_ms = 0;
    uint32_t starve_logged_ms = 0;
    uint32_t carried = 0;
    StreamOutcome outcome = StreamOutcome::NetLost;

    while (request_still_current(generation, station)) {
        const std::size_t buffered = xStreamBufferBytesAvailable(s_ring);
        if (buffered == 0) {
            if (s_net_done.load(std::memory_order_acquire)) {
                // 网络侧已死且环空：这一会话到头了，播放任务稍后重连。
                outcome = StreamOutcome::NetLost;
                break;
            }
            const uint32_t now = now_ms();
            if (starve_since_ms == 0) starve_since_ms = now;
            if (now - starve_logged_ms >= kStarveLogEveryMs) {
                starve_logged_ms = now;
                ESP_LOGW(kTag, "starved for %u ms",
                         static_cast<unsigned>(now - starve_since_ms));
            }
            if (now - starve_since_ms >= kStarveGiveUpMs) {
                ESP_LOGE(kTag, "starved too long, ending the session");
                outcome = StreamOutcome::NetLost;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        starve_since_ms = 0;

        const std::size_t want = std::min<std::size_t>(buffered, kInputSize - carried);
        if (want == 0) break;
        const std::size_t got =
            xStreamBufferReceive(s_ring, state.input + carried, want, kRingRecvTimeout);
        if (got == 0) continue;

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = state.input;
        raw.len = carried + static_cast<uint32_t>(got);
        raw.eos = 0;
        raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;
        while (raw.len > 0 && request_still_current(generation, station)) {
            esp_audio_simple_dec_out_t out = {};
            out.buffer = state.output;
            out.len = kOutputSize;
            const esp_audio_err_t dec_result =
                esp_audio_simple_dec_process(state.dec, &raw, &out);
            if (dec_result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGE(kTag, "decoder needs %u B, buffer has %u",
                         static_cast<unsigned>(out.needed_size),
                         static_cast<unsigned>(kOutputSize));
                raw.len = 0;
                break;
            }
            if (dec_result != ESP_AUDIO_ERR_OK &&
                dec_result != ESP_AUDIO_ERR_CONTINUE) {
                // 直播流丢一段帧头很常见（从中途接入）：丢掉这批输入继续收。
                ESP_LOGW(kTag, "MP3 decode failed: %d, resyncing",
                         static_cast<int>(dec_result));
                raw.len = 0;
                carried = 0;
                break;
            }
            if (raw.consumed > raw.len) raw.consumed = raw.len;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (out.decoded_size == 0) continue;

            if (!state.format_ready) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(state.dec, &info) ==
                        ESP_AUDIO_ERR_OK &&
                    info.sample_rate > 0) {
                    state.source_channels = info.channel > 0 ? info.channel : 1;
                    state.format_ready = true;
                    s_stream_hz.store(info.sample_rate, std::memory_order_release);
                    if (!audio_open_at(info.sample_rate)) {
                        ESP_LOGE(kTag, "cannot open codec at %u Hz",
                                 static_cast<unsigned>(info.sample_rate));
                        close_pipeline(&state);
                        wait_net_idle();
                        return StreamOutcome::Failed;
                    }
                    ESP_LOGI(kTag, "stream format: %u Hz, %u ch",
                             static_cast<unsigned>(info.sample_rate),
                             static_cast<unsigned>(state.source_channels));
                }
            }
            if (!state.format_ready) continue;

            const std::size_t mono_bytes =
                downmix_to_mono(state.output,
                                static_cast<std::size_t>(out.decoded_size),
                                state.source_channels);
            if (mono_bytes == 0) continue;
            if (!audio_write(state.output, mono_bytes)) {
                ++write_failures;
                if ((write_failures & 0x0F) == 1) {
                    ESP_LOGW(kTag, "audio write failed %u times (last %u B)",
                             static_cast<unsigned>(write_failures),
                             static_cast<unsigned>(mono_bytes));
                }
            } else {
                decoded_bytes_total += mono_bytes;
                s_listen_samples.fetch_add(mono_bytes / 2, std::memory_order_relaxed);
                last_audio_ms = now_ms();
                calculate_levels(state.output, mono_bytes);
                if (!played_anything) {
                    played_anything = true;
                    radio_ui_set_playback(RadioPlaybackState::Playing, "正在直播");
                }
            }

            const uint32_t now = now_ms();
            if (now - health_logged_ms >= kHealthLogEveryMs) {
                health_logged_ms = now;
                ESP_LOGI(kTag,
                         "health: decoded %u KB, ring %u KB, write_fail %u, "
                         "since_audio %u ms, heap %u B (min %u B)",
                         static_cast<unsigned>(decoded_bytes_total / 1024),
                         static_cast<unsigned>(xStreamBufferBytesAvailable(s_ring) / 1024),
                         static_cast<unsigned>(write_failures),
                         static_cast<unsigned>(now - last_audio_ms),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                         static_cast<unsigned>(
                             heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
            }
            if (now - last_audio_ms >= kSilenceRestartMs &&
                audio_restarts < kMaxAudioRestarts) {
                ESP_LOGW(kTag, "silent for %u ms — rebuilding the audio path",
                         static_cast<unsigned>(now - last_audio_ms));
                ++audio_restarts;
                if (!restart_audio(&state)) {
                    ESP_LOGE(kTag, "cannot rebuild the audio path");
                    close_pipeline(&state);
                    wait_net_idle();
                    return StreamOutcome::Failed;
                }
                last_audio_ms = now_ms();
            }
        }
        carried = request_still_current(generation, station) ? raw.len : 0;
        if (carried > 0 && carried < kInputSize) {
            std::memmove(state.input, raw.buffer, carried);
        } else if (carried >= kInputSize) {
            carried = 0;
        }
    }

    wait_net_idle();
    audio_close();
    close_pipeline(&state);

    if (!request_still_current(generation, station)) return StreamOutcome::Dropped;
    if (!played_anything) return StreamOutcome::Failed;
    return outcome;
}

StreamOutcome run_session(std::size_t station, uint32_t generation) {
    if (!request_still_current(generation, station)) return StreamOutcome::Dropped;
    radio_ui_set_playback(RadioPlaybackState::Connecting, "正在连接电台");
    return play_stream(station, generation);
}

void player_task(void *) {
    std::size_t last_attempted_station = SIZE_MAX;
    uint8_t failed_attempts = 0;
    while (true) {
        if (!s_network_connected.load(std::memory_order_acquire)) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "等待网络");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_wanted_playing.load(std::memory_order_acquire)) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        const std::size_t station = s_station_index.load(std::memory_order_acquire);
        const uint32_t generation = s_generation.load(std::memory_order_acquire);
        if (station != last_attempted_station) {
            last_attempted_station = station;
            failed_attempts = 0;
        }
        s_stream_active.store(true, std::memory_order_release);
        const StreamOutcome outcome = run_session(station, generation);
        s_stream_active.store(false, std::memory_order_release);
        if (!request_still_current(generation, station)) continue;

        if (outcome == StreamOutcome::Dropped) {
            failed_attempts = 0;
            radio_ui_set_playback(RadioPlaybackState::Reconnecting, "已暂停");
            vTaskDelay(pdMS_TO_TICKS(1200));
            continue;
        }

        ++failed_attempts;
        s_generation.fetch_add(1, std::memory_order_acq_rel);
        if (failed_attempts >= kMaxFailedAttempts && MR_STATION_COUNT > 1) {
            // 死台自救：连着几轮连不上就跳下一台（下一台若也死，继续跳）。
            ESP_LOGW(kTag, "station %s unreachable, moving to the next one",
                     MR_STATIONS[station].name);
            radio_ui_set_playback(RadioPlaybackState::Error, "暂时连不上 已换下一台");
            vTaskDelay(pdMS_TO_TICKS(1500));
            failed_attempts = 0;
            radio_player_select_relative(1);
            continue;
        }
        radio_ui_set_playback(RadioPlaybackState::Error, "连接失败 正在重试");
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

bool ensure_ring() {
    if (s_ring) return true;
    s_ring = xStreamBufferCreateStatic(kRingBytes, 1, s_ring_storage, &s_ring_struct);
    if (s_ring) {
        ESP_LOGI(kTag, "ring buffer: %u B (PSRAM)", static_cast<unsigned>(kRingBytes));
        return true;
    }
    ESP_LOGE(kTag, "cannot create ring buffer");
    return false;
}

}  // namespace

bool radio_player_init() {
    s_station_mutex = xSemaphoreCreateMutex();
    if (!s_station_mutex) return false;
    if (!ensure_ring()) return false;

    const uint8_t saved_station = restore_station_index();
    const uint8_t saved_volume = load_u8("volume", 80);
    s_station_index.store(saved_station);
    s_volume.store(std::clamp<uint8_t>(saved_volume, 10, 100));
    s_favorites.store(load_u32("favorites", 0), std::memory_order_release);

    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) return false;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;

    radio_ui_set_station(s_station_index.load(), radio_player_station_count(),
                         radio_player_station(s_station_index.load()));
    radio_ui_set_volume(s_volume.load());
    radio_ui_set_favorites(s_favorites.load());
    if (xTaskCreate(player_task, "mr_play", 7168, nullptr, 6, &s_player_task) != pdPASS) {
        s_player_task = nullptr;
        return false;
    }
    return true;
}

void radio_player_set_network(bool connected) {
    s_network_connected.store(connected, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void radio_player_release_ring() {
    if (!s_ring) return;
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    radio_player_set_playing(false);
    const uint32_t deadline = now_ms() + kHttpTimeoutMs + 1500;
    while (s_net_active.load(std::memory_order_acquire) && now_ms() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 静态环不能删，只能清空：配网结束后的第一次播放直接复用。
    xStreamBufferReset(s_ring);
}

void radio_player_toggle() {
    radio_player_set_playing(!radio_player_is_playing());
}

void radio_player_set_playing(bool playing) {
    s_wanted_playing.store(playing, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (playing) {
        s_listen_samples.store(0, std::memory_order_release);
        clear_levels();
    } else {
        radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
    }
}

bool radio_player_is_playing() {
    return s_wanted_playing.load(std::memory_order_acquire);
}

bool radio_player_wait_idle(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_stream_active.load(std::memory_order_acquire)) {
        if (static_cast<int32_t>(deadline - xTaskGetTickCount()) <= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void radio_player_select_relative(int delta) {
    const uint8_t current =
        static_cast<uint8_t>(s_station_index.load(std::memory_order_acquire));
    const uint8_t next = delta >= 0 ? mr_station_next(current)
                                    : mr_station_prev(current);
    radio_player_set_station(next);
}

void radio_player_set_station(std::size_t index) {
    if (index >= MR_STATION_COUNT) index = 0;
    s_stream_hz.store(0, std::memory_order_release);
    s_station_index.store(index, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    persist_station(static_cast<uint8_t>(index));
    radio_ui_set_station(index, radio_player_station_count(),
                         radio_player_station(index));
    if (radio_player_is_playing()) {
        radio_ui_set_playback(RadioPlaybackState::Connecting, "正在切换电台");
    }
}

uint32_t radio_player_listen_ms() {
    const uint64_t samples = s_listen_samples.load(std::memory_order_relaxed);
    const uint32_t rate = s_stream_hz.load(std::memory_order_relaxed);
    if (rate == 0) return 0;
    const uint64_t ms = samples * 1000ULL / rate;
    return ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(ms);
}

uint32_t radio_player_stream_hz() {
    return s_stream_hz.load(std::memory_order_relaxed);
}

std::size_t radio_player_station_index() {
    return s_station_index.load(std::memory_order_acquire);
}

std::size_t radio_player_station_count() {
    return MR_STATION_COUNT;
}

mr_station_snapshot_t radio_player_station(std::size_t index) {
    return station_snapshot(index);
}

void radio_player_set_volume(uint8_t volume) {
    volume = std::clamp<uint8_t>(volume, 10, 100);
    s_volume.store(volume, std::memory_order_release);
    audio_apply_volume();
    save_u8("volume", volume);
    radio_ui_set_volume(volume);
}

uint8_t radio_player_volume() {
    return s_volume.load(std::memory_order_acquire);
}

uint32_t radio_player_favorites() {
    return s_favorites.load(std::memory_order_acquire);
}

void radio_player_set_favorite(std::size_t index, bool on) {
    if (index >= MR_STATION_COUNT) return;
    uint32_t favorites = s_favorites.load(std::memory_order_acquire);
    if (on) {
        favorites |= (1u << index);
    } else {
        favorites &= ~(1u << index);
    }
    s_favorites.store(favorites, std::memory_order_release);
    save_u32("favorites", favorites);
    radio_ui_set_favorites(favorites);
}

uint8_t radio_player_levels(uint8_t *levels, uint8_t capacity) {
    if (!levels) return 0;
    const uint8_t n = static_cast<uint8_t>(std::min<std::size_t>(
        capacity, MR_LEVEL_COUNT));
    for (uint8_t i = 0; i < n; ++i) {
        levels[i] = s_levels[i].load(std::memory_order_relaxed);
    }
    return n;
}
