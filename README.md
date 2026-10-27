# music-radio · 音乐电台（ESP-Mosaico 版）

> **独立镜像仓库**：本工程是 [esp-mosaico-vibe](https://github.com/) 工作区里
> `projects/music-radio` 的镜像。它不能单独构建 —— 构建依赖工作区的
> `submodule/esp-mosaico-utils`、`submodule/esp-mosaico-bsp`（Recovery 契约、
> BSP、低噪构建脚本）与 `mosaico.py` 设备工具链。components/ 里的本地副本
> （esp-wifi-connect / esp_audio_codec / cjson）是为绕过组件 CDN 403 而内置的，
> esp_audio_codec 只保留了 esp32s31 目标的预编译库。

装在 **ESP-Mosaico 小方块屏**（480×480 触摸，esp32s31 + ES8311）上的互联网音乐电台，
台源为 [蜻蜓 FM](https://www.qtfm.cn/) 的 18 个纯音乐直播台。参考
FoloToy AI Passport 上的 music-radio（三键 240×320 版）重新设计：
更大的触摸屏、更完整的交互、为音乐重标定的音频链路。

**2026-10-24 全部 18 台实测在线**（纯 HTTP GET 收流 200，MPEG1 L3
44.1 kHz joint-stereo 64 kbps）。

## 相比参考版的升级

| 维度 | 参考（FoloToy 三键版） | 本版 |
|---|---|---|
| 屏幕 | 240×320 + 三键 | 480×480 触摸：调谐盘轻点换台、列表点播、收藏星标 |
| 主题 | 霓虹靛蓝 | 霓虹靛蓝升级：分组配色刻度盘、18 条青→靛渐变 VU 律动条 |
| 音频缓冲 | 16 KB（约 2 s） | **48 KB PSRAM 环（约 6 s 抖动容忍）** |
| 音量曲线 | -50..0 dB 默认 | **-44..+4 dB 按音乐响度重标定**（广播流峰值贴近满量程，避免削波） |
| 时钟 | 无 | SNTP 自动对时（顶栏 HH:MM，东八区） |
| 收藏 | 无 | 星标收藏（NVS 持久化），列表页一键过滤 |
| 记忆 | 台 + 音量 | 台（按台名恢复，清单升级不串台）+ 音量 + 收藏 |
| 睡眠定时 | 无 | 关/15/30/60/90 分钟，到点停播熄屏深睡 |
| 配网 | 按键 + AP | 触摸屏扫描选网 + 软键盘（esp-wifi-connect，english-podcast 真机同款） |

## 电台

| 组 | 台 |
|---|---|
| 流行 | 动感101, LoveRadio, 动听音乐, 星河音乐, 清晨音乐 |
| 经典 | 怀旧好声音, 江苏经典, 两广音乐 |
| 亚洲 | HD音乐, 亚洲天空, 亚洲粤语, 亚洲经典 |
| 省市 | 广东音乐, 顺德音乐, 怀集音乐, 苏州音乐, 河北音乐, 哈尔滨音乐 |

清单在 `main/mr_radio.c`（纯 C11，宿主测试直接编译）；调谐盘位置按四组分段铺在
87.5–108 MHz 刻度上（台与台不重合即可，不冒充真实 FM 频率）。平台下架某台时
改清单后重跑 `python tools/gen_font.py`（台名字模）。

## 怎么用（全触摸）

1. **第一次**：开机自动开热点 `Music Radio`，手机连上（或直接在设备屏上
   扫描选网 → 软键盘输密码），连上自动保存并重启。
2. **收音机主页**：刻度盘左/右 1/3 轻点 = 上一台/下一台；分组 chip 跳组；
   点 `播放` 开播（正在缓冲约 2 s 后出声）；点 `音量` 弹音量层（± 调节，
   1.6 s 自动收起）；点 `电台` 进列表。
3. **电台列表**：分组/收藏过滤，行尾星标收藏，点行即播。
4. **设置**：WiFi 配网、音量、睡眠定时、亮度/熄屏、返回。

断线自动重连（网络任务 5 轮退避重试 + 播放任务持续重连）；某台连不上
4 轮自动跳下一台。上次的台、音量、收藏重启后都在。

## 播放管线（english-podcast 血统，按直播改造）

```
蜻蜓 FM 直播流 ──HTTP──▶ 48 KB PSRAM 预取环 ──▶ esp_audio_simple_dec (MP3 软解)
 (64 kbps)                ≈6 秒抖动余量            │44.1 kHz 立体声
                          失败 5 轮退避重连         ▼
                              (L+R)/2 等权 downmix ──▶ ES8311（音量曲线 -44..+4 dB）
```

- 预取 16 KB（约 2 s）再开播，环空 12 s 才放弃本会话；
- 音频通路看门狗：缓冲有数据但喇叭 5 s 无声 → 自动重建 codec 通路；
- VU 能级在解码线程按 RMS→dB 计算，UI 200 ms 拉取（快攻慢释动画）。

## 构建 / 测试 / 上机

```bash
# 宿主测试（纯 C11，无 IDF 依赖）
D:/esp-mo/mingw/w64devkit/bin/gcc.exe -std=c11 -Wall -Wextra -I main \
    tests/test_mr_radio.c main/mr_radio.c -o tests/test_mr_radio.exe

# 字库（改界面文案后必须重跑）
python tools/gen_font.py --check

# 固件（首次约 20-25 分钟；增量脚本已处理 .ninja_deps/MSYSTEM/ccache 坑）
powershell -ExecutionPolicy Bypass -File build_mr.ps1

# 上机（Retained Recovery 契约；system-update 处理新布局/首装）
python mosaico.py iris system-update --project projects/music-radio
```

## 真机验收清单（待设备）

- [ ] 首配：热点 `Music Radio` → 屏上扫描选网 → 软键盘输密码 → 自动重启联网
- [ ] 主页出声：18 台逐台能播（重点听「哈尔滨音乐」「怀集音乐」等冷门台）
- [ ] 出声延迟 ≤ 4 s；Wi-Fi 停顿 3-5 s 内听感不断流（48 KB 环生效）
- [ ] VU 条随音乐跳动，暂停后平滑衰减归零
- [ ] 音量弹层 ± 生效、1.6 s 自动收起；重启后音量/台/收藏保留
- [ ] 刻度盘轻点左右换台、分组 chip 跳组、列表星标/点播
- [ ] 断网（拔路由）：状态转「连接失败 正在重试」，恢复后自动续播
- [ ] 睡眠定时到点停播熄屏；亮度/熄屏档生效
- [ ] 顶栏时钟与实际时间一致（±1 分钟）
- [ ] 真机回归：正常 → Vibe Mode → 正常，Iris OTA 通道不受影响

## 已知边界

- 台源与流归蜻蜓 FM，仅供个人学习。
- **蜻蜓对长连接按接近实时码率限速**（实测 PC 持续拉流 ≈8.9 KB/s vs 消费 8.0 KB/s，
  余量仅 11%）。固件用 192 KB 静态 PSRAM 环（约 24 秒余量）+ 64 KB 预缓冲 +
  10 秒读超时把它压稳；真机连续播放零 starve（2026-10-27）。网络极差时仍可能
  短暂缓冲，属于源头上限。
- ES8311 为单声道 DAC，输出为等权立体声 downmix（不做假立体声）。
- 蜻蜓无 128 kbps 变体（实测 404），64 kbps 是该平台音乐台的上限。
- 冷门 SSID 汉字可能显示为空框（字库只烘高频表，english-podcast 同策略）。
- 构建尾部会报一条 `factory too small (overflow ...)` 警告：固件 2.4 MB
  超过 factory 槽（1.75 MB，按 immutable_layout 契约固定留给 Recovery）。
  应用经 `iris system-update` / `app-update` 装进 ota_0（14.6 MB），该警告符合
  预期，**不要**为此放大 factory（known-issues #34）。

