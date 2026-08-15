# ESP32-S3 DLNA 音频流 — 编译烧录指南

> 当前版本：v4.2  |  仓库：`MYHealer/dlna-esp32` master 分支

## 环境信息

| 项目 | 值 |
|---|---|
| 芯片 | ESP32-S3 (Xtensa LX7 双核) |
| IDF 版本 | v5.5.4 |
| ADF 路径 | `E:\ESP\.espressif\v5.5.4\esp-adf`（已废弃，项目已迁移到 GMF） |
| Python 环境 | `E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env` |
| 串口 | COM31（根据实际设备调整） |
| 项目路径 | `E:\ESP\dlna` |
| 分区表 | `partitions_dlna_example.csv` |

## 必须用 PowerShell

**不要用 Git Bash、CMD 或 WSL。** ESP-IDF 的环境脚本（`export.ps1`）是 PowerShell 格式，其他 shell 会报编码或路径错误。

## 编译

```powershell
# 方法1：直接运行项目脚本（推荐，一条命令）
powershell -ExecutionPolicy Bypass -File E:\ESP\dlna\build.ps1

# 方法2：手动
. "E:\ESP\esp_env.ps1"
Set-Location "E:\ESP\dlna"
& $PYTHON $IDF_PY build
```

## 烧录

```powershell
# 一条命令完成增量编译 + 烧录（推荐）
powershell -ExecutionPolicy Bypass -File E:\ESP\dlna\flash.ps1

# 手动
. "E:\ESP\esp_env.ps1"
Set-Location "E:\ESP\dlna"
& $PYTHON $IDF_PY -p COM31 flash
```

**注意：** `flash.ps1` 会先编译再烧录，不需要分两步。

## 串口监控

```powershell
. "E:\ESP\esp_env.ps1"
Set-Location "E:\ESP\dlna"
& $PYTHON $IDF_PY -p COM31 monitor
```

按 `Ctrl+]` 退出监控。**烧录前必须先退出监控**，否则串口被占用会失败。

## 环境变量说明

所有环境变量统一在 `E:\ESP\esp_env.ps1`，由 `build.ps1` / `flash.ps1` 自动加载：

```powershell
$env:IDF_TOOLS_PATH    = "E:\ESP\.espressif\tools"
$env:IDF_PATH           = "E:\ESP\.espressif\v5.5.4\esp-idf"
$env:ADF_PATH           = "E:\ESP\.espressif\v5.5.4\esp-adf"
$env:IDF_PYTHON_ENV_PATH = "E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env"
$env:ESP_ROM_ELF_DIR    = "E:\ESP\.espressif\tools\idf_rom_elfs"
```

`idf.py` 和 `python.exe` 用绝对路径调用，不依赖 PATH：

```powershell
$PYTHON  = "E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env\Scripts\python.exe"
$IDF_PY  = "E:\ESP\.espressif\v5.5.4\esp-idf\tools\idf.py"
```

## 项目结构

```
E:\ESP\dlna\
  main/
    dlna.c              — 主程序：WiFi、DLNA、UI、GMF 管线
    custom_dlna.c       — DLNA 协议层（HTTP + SSDP）
  components/
    custom_dlna/        — DLNA 组件（UPnP/SSDP/HTTP）
    gmf_audio/          — GMF 音频编解码（AAC/MP3/FLAC 等）
    gmf_core/           — GMF 核心框架
    gmf_io/             — GMF I/O 元素（I2S 输出等）
    gmf_loader/         — GMF 加载器
    esp_codec_dev/      — 芯片音频编解码驱动
    i2s_pa/             — I2S 功放驱动
    lvgl/               — LVGL 图形库
    lvgl_port/          — LVGL 显示端口
    tft_display/        — TFT 显示驱动
    rotary_encoder/     — 旋钮编码器
    lyrics_fetch/       — 歌词获取
    jsmn/               — JSON 解析器
    nghttp/             — HTTP 客户端
    idf6_compat/        — IDF 6.x 兼容层
    esp-adf-libs/       — ESP 音频库（旧，逐步替换）
  flash.ps1             — 一键编译+烧录
  build.ps1             — 仅编译
  sdkconfig.defaults    — 默认配置
  sdkconfig.defaults.esp32s3 — ESP32-S3 专用配置
  partitions_dlna_example.csv — 分区表
```

## 常见问题

### Flash 端口占用
```
A fatal error occurred: Serial port COM31 is not available
```
→ 先关掉串口监控（`idf.py monitor` 或其他串口工具），再烧录。

### 编译报找不到 idf.py
```
idf.py : 无法将"idf.py"识别为 cmdlet
```
→ 没有加载环境变量。确保用 PowerShell 运行 `build.ps1` 或先执行 `. "E:\ESP\esp_env.ps1"`。

### 中文乱码
→ `esp_env.ps1` 已设置 `$env:PYTHONUTF8 = "1"`，如果仍有问题检查终端编码为 UTF-8。
