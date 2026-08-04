# ESP32 DLNA MediaRenderer — 网易云音乐投屏播放器

> 基于 ESP-ADF 的 DLNA 音频渲染器，支持网易云/QQ音乐/酷狗等 App 投屏播放，PCM5102A DAC 高品质输出。

## 适用人群

- **音乐爱好者** — 想用手机投屏到桌面音箱/功放，享受高品质音频
- **DIY 玩家** — 手上有 ESP32-S3 + PCM5102A 开发板，想折腾 DLNA 音频流
- **不想买成品** — 不想花几百块买蓝牙音箱，用 ESP32 自制 WiFi 音频接收器

## 功能

- ✅ **DLNA 投屏播放** — 网易云/QQ音乐/酷狗等支持 DLNA 的 App 均可投屏
- ✅ **高品质音频** — PCM5102A DAC（112dB 动态范围），MCLK 直供低 jitter
- ✅ **HTTP 流解码** — MP3/AAC/WAV/FLAC/OGG，1MB PSRAM 缓冲防卡顿
- ✅ **采样率自动跟随** — 44.1kHz/48kHz 自动切换，零重采样
- ✅ **旋钮控制** — 旋转编码器调节音量，按压暂停/播放
- ✅ **GENA 事件同步** — 手机端实时显示播放状态，歌曲自动切换
- ✅ **双核架构** — Core0 网络 + Core1 音频，互不干扰
- ✅ **软音量控制** — ALC 增益映射 + 50ms 防抖，旋钮手感丝滑

## 工作原理

```
┌──────────────────────────────────────────────────────────────┐
│                         手机 (网易云)                         │
│  ┌────────────┐    ┌──────────────────┐    ┌──────────────┐  │
│  │ 选择投屏    │───▶│  DLNA 控制协议    │───▶│  HTTP 代理    │  │
│  │ "MS01B"    │    │  SetAVTransport  │    │  (音频流 URL)  │  │
│  └────────────┘    └────────┬─────────┘    └──────┬───────┘  │
│                             │                     │          │
└─────────────────────────────┼─────────────────────┼──────────┘
                              │ SOAP                │ HTTP 流
                              ▼                     ▼
┌──────────────────────────────────────────────────────────────┐
│                       ESP32-S3 (渲染器)                       │
│                                                              │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐  │
│  │ SSDP     │   │ HTTP     │   │ 解码器    │   │ I2S      │  │
│  │ 设备发现  │   │ 流拉取   │   │ MP3/AAC  │   │ PCM5102A │  │
│  │ (Core0)  │   │ (Core0)  │   │ (Core1)  │   │ (Core1)  │  │
│  └──────────┘   └──────────┘   └──────────┘   └──────────┘  │
│                                                              │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                 │
│  │ GENA     │   │ SOAP     │   │ 旋钮     │                 │
│  │ 事件通知  │   │ 控制接口  │   │ 音量/暂停 │                 │
│  │ (Core0)  │   │ (Core0)  │   │ (GPIO)   │                 │
│  └──────────┘   └──────────┘   └──────────┘                 │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼ I2S (BCK+LRCK+DIN+MCLK)
┌──────────────────────────────────────────────────────────────┐
│                    PCM5102A DAC 模块                          │
│                    → 功放 / 耳机 / 音箱                       │
└──────────────────────────────────────────────────────────────┘
```

## 支持的开发板

| 芯片 | 常见开发板 | 架构 | Flash | PSRAM | 状态 |
|------|-----------|------|-------|-------|------|
| **ESP32-S3** | ESP32-S3-DevKitC-1, ESP32-S3-WROOM-1 (N16R8) | Xtensa 双核 | 16MB | 8MB (Octal) | ✅ 已验证 |
| **ESP32-S31** | ESP32-S31-DevKitC | RISC-V 单核 | 16MB | 8MB | 🔧 预适配 |
| **ESP32** | ESP32-DevKitC, NodeMCU-32S | Xtensa 双核 | 4MB+ | 可选 | ⚠️ 内存紧张 |
| **ESP32-C3** | ESP32-C3 SuperMini | RISC-V 单核 | 4MB | ❌ | ❌ 不支持（无 PSRAM） |

> ⚠️ **Flash/PSRAM 要求**
> - 推荐 **16MB Flash + 8MB PSRAM**（ESP32-S3-WROOM-1 N16R8）
> - 最低 **4MB Flash + 2MB PSRAM**（需要减小缓冲区）
> - 无 PSRAM 的芯片（如 ESP32-C3）**不支持**（1MB HTTP 缓冲需要 PSRAM）

### ESP32-S31 适配说明

ESP32-S31 是 RISC-V 架构，与 ESP32-S3（Xtensa）的差异：

| 差异项 | ESP32-S3 (Xtensa) | ESP32-S31 (RISC-V) |
|--------|-------------------|---------------------|
| 工具链 | `xtensa-esp-elf` | `riscv32-esp-elf` |
| I2S 外设 | I2S0 + I2S1 | I2S0 |
| GPIO 映射 | 任意 GPIO | 任意 GPIO |
| PSRAM | Octal PSRAM | Octal PSRAM |
| 编译目标 | `esp32s3` | `esp32s31` |

适配步骤：
1. `idf.py set-target esp32s31`
2. 检查 `sdkconfig.defaults` 中 PSRAM 配置
3. I2S GPIO 映射不变（GPIO15/16/7/17）

## 硬件

### 组件清单

| 组件 | 型号 | 参考价格 |
|------|------|---------|
| 主控 | ESP32-S3-WROOM-1 (N16R8) | ¥25-35 |
| DAC | PCM5102A 模块（自带匹配电阻） | ¥8-15 |
| 编码器 | 旋转编码器模块 | ¥3-5 |
| 连接线 | 杜邦线若干 | ¥2 |

**总成本约 ¥40-60**，远低于成品 WiFi 音频接收器。

### 接线

```
ESP32-S3                    PCM5102A 模块
┌──────────┐                ┌──────────┐
│          │                │          │
│  GPIO15  │────────────────│  BCK     │
│  GPIO16  │────────────────│  LRCK    │
│  GPIO7   │────────────────│  DIN     │
│  GPIO17  │────────────────│  SCK     │
│  GND     │────────────────│  GND     │
│  3.3V    │────────────────│  VCC     │
│          │                │          │
└──────────┘                └──────────┘

ESP32-S3                    旋转编码器
┌──────────┐                ┌──────────┐
│  GPIO4   │────────────────│  CLK     │
│  GPIO5   │────────────────│  DT      │
│  GPIO6   │────────────────│  SW      │
│  GND     │────────────────│  GND     │
│  3.3V    │────────────────│  VCC     │
└──────────┘                └──────────┘
```

| ESP32-S3 GPIO | PCM5102A 引脚 | 说明 |
|---------------|--------------|------|
| GPIO15 | BCK | 位时钟 (BCLK) |
| GPIO16 | LRCK | 左右声道时钟 (WS) |
| GPIO7 | DIN | 音频数据 |
| GPIO17 | SCK | MCLK（256×采样率，自动跟随） |

> 💡 **MCLK 说明**
> - PCM5102A 的 SCK 引脚可以接 GND（内部 PLL 恢复时钟）或接 MCLK（外部时钟）
> - 接 MCLK 时钟抖动更低（~10-20ps vs ~50ps），音质更优
> - 本项目默认使用 MCLK（GPIO17），PCM5102A 模块自带匹配电阻，无需外接

## 快速开始

### 方式一：下载固件直接烧录（推荐）

从 [Releases](https://github.com/MYHealer/dlna-esp32/releases) 下载最新固件包。

**使用 esptool 烧录：**

```bash
# 安装 esptool
pip install esptool

# 烧录（ESP32-S3，COM31 按实际修改）
esptool --chip esp32s3 -p COM31 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 dlna.bin
```

**使用乐鑫 Flash Download Tool 烧录（Windows GUI）：**

1. 下载 [Flash Download Tool](https://www.espressif.com/en/support/download/other-tools)
2. 选择芯片类型：ESP32-S3
3. 加载三个文件：
   - `bootloader.bin` → 地址 `0x0`
   - `partition-table.bin` → 地址 `0x8000`
   - `dlna.bin` → 地址 `0x10000`
4. 设置：SPI Speed 80MHz, SPI Mode DIO, Flash Size 16MB
5. 点击 START

### 方式二：从源码构建

需要 ESP-IDF v5.5.4 + ESP-ADF v5.5.4 环境。

```powershell
# 编译
.\build.ps1

# 烧录
.\flash.ps1

# 串口监控
python serial_monitor.py
```

## 使用步骤

**① 烧录固件**

按上方方法烧录固件到 ESP32-S3 开发板。

**② 连接硬件**

按接线图连接 PCM5102A 模块和旋转编码器。PCM5102A 输出接功放或耳机。

**③ 连接 WiFi**

ESP32-S3 上电后自动连接 WiFi（需在 `sdkconfig.defaults` 中配置 WiFi 名称和密码）。

**④ 投屏播放**

1. 手机连接同一 WiFi
2. 打开网易云音乐，播放一首歌
3. 点击播放界面右上角的 **投屏按钮**
4. 选择设备 **"MS01B"**
5. 音乐从 ESP32-S3 + PCM5102A 输出

**⑤ 控制**

- **旋钮旋转** — 调节音量
- **旋钮按压** — 暂停/播放
- **手机端** — 可切换歌曲、调节音量（前台时）

## 技术要点

### 歌曲时长解析

`esp_audio_duration_get` 返回的是**解码累计时间**（微秒），**不是歌曲总时长**！

歌曲时长从 DIDL-Lite metadata 解析：
```
<res duration="00:03:45" ...>http://...</res>
```
→ 3 分 45 秒 = 225 秒

### GENA 订阅策略

DLNA 切歌依赖 GENA 事件：歌曲播完 → 发 `STOPPED` 事件 → 网易云推下一首。

关键：HTTP 通知失败时**保留订阅 URL**，只清理 TCP 连接，下一条通知自动重连。不能删除订阅，否则歌曲播完时无法通知手机。

### 双核分工

| Core | 任务 | 优先级 |
|------|------|--------|
| Core0 | WiFi + HTTP 拉流 + DLNA 协议 | 内核 + 5-6 |
| Core1 | 音频解码 + I2S 输出 | 8-9（最高） |
| Core1 | 采样率跟随 | 4 |

I2S 输出优先级最高（9），确保 DMA buffer 永不饿死。

### HTTP 缓冲策略

```
网易云 CDN → [1MB PSRAM 缓冲] → 解码器 → [256KB I2S 缓冲] → PCM5102A
                ↑                              ↑
           吸收网络抖动                    防止 DMA underrun
```

- HTTP 请求块 32KB，减少 RTT
- User-Agent 伪装 Chrome 浏览器，避免降速
- 采样率动态跟随，零重采样省 CPU

## 常见问题

### Q: 投屏后没有声音？

检查接线：PCM5102A 的 XMT 引脚必须拉高（接 3.3V 或模块已处理）。

### Q: 播放卡顿？

- 确认 WiFi 信号稳定（2.4GHz）
- 确认 PSRAM 已启用（`sdkconfig` 中 `CONFIG_SPIRAM=y`）
- 串口日志看是否有 `HTTP_STREAM` 错误

### Q: 手机搜不到设备？

- 确认手机和 ESP32 在同一 WiFi
- 等待 30 秒（SSDP 发现需要时间）
- 重启网易云 App

### Q: 播放几首歌后不自动切歌？

查看串口日志，确认 `GENA OK` 是否持续输出。如果出现 `GENA notify failed`，说明网络不稳定，设备会自动重连。

## 参考项目

- [miair-next](https://github.com/) — Python DLNA 代理，GENA 事件管理参考
- [Jw-Y1](https://github.com/) — 同款 PCM5102A 硬件 ESP32 渲染器
- [schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) — 音频播放库参考

## 许可证

MIT
