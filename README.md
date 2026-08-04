# ESP32-S3 DLNA MediaRenderer

网易云音乐投屏播放器 — 基于 ESP32-S3 + PCM5102A + ESP-ADF

## 功能

- DLNA 投屏播放（网易云/QQ音乐/酷狗等）
- HTTP 流解码（MP3/AAC/WAV/FLAC/OGG）
- I2S PCM5102A DAC 输出（MCLK 直供，低 jitter）
- 旋钮编码器控制（音量调节 + 暂停/播放）
- 采样率动态跟随（零重采样，44.1kHz/48kHz 自动切换）
- GENA 事件订阅（状态同步 + 歌曲自动切换）
- PSRAM 大缓冲（1MB HTTP + 256KB I2S，防卡顿）
- 软音量控制（ALC 增益映射 + 防抖）

## 硬件

| 组件 | 型号 |
|------|------|
| 主控 | ESP32-S3-WROOM-1 (N16R8) |
| DAC | PCM5102A 模块 |
| 编码器 | 旋转编码器（CLK=4, DT=5, SW=6） |

### 接线

| ESP32-S3 | PCM5102A | 说明 |
|----------|----------|------|
| GPIO15 | BCK | 位时钟 |
| GPIO16 | LRCK | 左右声道时钟 |
| GPIO7 | DIN | 音频数据 |
| GPIO17 | SCK | MCLK（256×fs） |

## 构建

需要 ESP-IDF v5.5.4 + ESP-ADF v5.5.4 环境。

```powershell
# 编译
.\build.ps1

# 烧录
.\flash.ps1

# 串口监控
python serial_monitor.py
```

## 技术要点

### 歌曲时长解析

`esp_audio_duration_get` 返回的是**解码累计时间**（微秒），不是歌曲总时长。
歌曲时长从 DIDL-Lite metadata 的 `duration="HH:MM:SS"` 属性解析。

### GENA 订阅策略

参考 miair-next：HTTP 通知失败时**保留订阅 URL**，只清理 TCP 连接。
下一条通知自动重连，订阅自然过期（30 分钟 timeout）。

### 双核分工

| Core | 任务 |
|------|------|
| Core0 | WiFi + HTTP 拉流 + DLNA 协议（SSDP/GENA/SOAP） |
| Core1 | 音频解码 + I2S 输出 + 采样率跟随 |

## 参考项目

- [miair-next](https://github.com/) — Python DLNA 代理，GENA 事件管理参考
- [Jw-Y1](https://github.com/) — 同款 PCM5102A 硬件 ESP32 渲染器
- [schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) — 音频播放库参考

## 许可证

MIT
