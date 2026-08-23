# ESP32-S3 DLNA 音频流 — 编译烧录指南

> 当前版本：v4.2  |  仓库：`MYHealer/dlna-esp32` master 分支

## 环境信息

| 项目 | 值 |
|---|---|
| 芯片 | ESP32-S3 (Xtensa LX7 双核) |
| IDF 版本 | v5.5.4（**不是** master/v6.x） |
| IDF 路径 | `E:\ESP\.espressif\v5.5.4\esp-idf` |
| Python 环境 | `E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env` |
| 串口 | COM31（根据实际设备调整） |
| 项目路径 | `E:\ESP\dlna` |
| 分区表 | `partitions_dlna_example.csv` |
| 固件大小 | ~5.5MB（16MB Flash，分区 7MB，剩余 ~22%） |

## 快速编译烧录（一行命令）

**PowerShell（推荐）：**
```powershell
# 仅编译
powershell -ExecutionPolicy Bypass -File E:\ESP\dlna\build.ps1

# 编译 + 烧录（一条命令，不需要分两步）
powershell -ExecutionPolicy Bypass -File E:\ESP\dlna\flash.ps1
```

**CMD.exe（备选，Git Bash 不可用）：**
```cmd
E:\ESP\dlna\build4.bat
```
> `build4.bat` 会自动设置 MSYSTEM、IDF_PATH、Python PATH，然后调用 `idf.py build`。
> 烧录：修改 bat 最后一行 `build` 改为 `-p COM31 flash`。

**⚠️ Git Bash 不可用：** `MSYSTEM=MINGW64` 会导致 idf.py 拒绝执行。必须用 PowerShell 或 CMD。

## 手动编译步骤

如果脚本不可用，按以下步骤手动操作：

```powershell
# 1. 加载环境（所有变量在 esp_env.ps1 中定义）
. "E:\ESP\esp_env.ps1"

# 2. 进入项目目录
Set-Location "E:\ESP\dlna"

# 3. 编译（用绝对路径调用，不依赖 PATH）
& "E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env\Scripts\python.exe" `
  "E:\ESP\.espressif\v5.5.4\esp-idf\tools\idf.py" build

# 4. 烧录
& "E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env\Scripts\python.exe" `
  "E:\ESP\.espressif\v5.5.4\esp-idf\tools\idf.py" -p COM31 flash
```

## 串口监控

```powershell
# PowerShell
. "E:\ESP\esp_env.ps1"
& "E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env\Scripts\python.exe" `
  "E:\ESP\.espressif\v5.5.4\esp-idf\tools\idf.py" -p COM31 monitor

# 或用 Python 直接读（适合 AI 脚本抓日志）
python -c "import serial,time,sys; sys.stdout.reconfigure(errors='replace'); s=serial.Serial('COM31',115200,timeout=3); [print(s.readline().decode('utf-8',errors='replace').rstrip()) for _ in range(100)]"
```

按 `Ctrl+]` 退出监控。**烧录前必须先退出监控**，否则串口被占用会失败。

## 环境变量（完整列表）

所有环境变量统一在 `E:\ESP\esp_env.ps1`，由 `build.ps1` / `flash.ps1` 自动加载：

| 变量 | 值 | 说明 |
|---|---|---|
| `IDF_TOOLS_PATH` | `E:\ESP\.espressif\tools` | IDF 工具链根目录 |
| `IDF_PATH` | `E:\ESP\.espressif\v5.5.4\esp-idf` | IDF SDK（**必须是 v5.5.4**，不是 master） |
| `ADF_PATH` | `E:\ESP\.espressif\v5.5.4\esp-adf` | 已废弃，项目已迁移到 GMF |
| `IDF_PYTHON_ENV_PATH` | `E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env` | Python 虚拟环境 |
| `ESP_ROM_ELF_DIR` | `E:\ESP\.espressif\tools\idf_rom_elfs` | ROM ELF 文件 |
| `PYTHONUTF8` | `1` | 防止中文乱码 |
| `MSYSTEM` | （必须为空） | Git Bash 会设为 MINGW64，导致 idf.py 报错 |

**关键：** `idf.py` 和 `python.exe` 用绝对路径调用，不依赖 PATH：

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
    miplay/             — MiPlay 小米妙播协议（mDNS + TCP 8899 + SafetyAuth 握手）
      miplay.c          — 核心协议实现（帧编解码、握手状态机、AES-CBC）
      include/miplay.h  — 头文件
      CMakeLists.txt    — 依赖: mdns lwip esp_wifi esp_netif esp_timer mbedtls
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
  build.ps1             — 仅编译（PowerShell）
  flash.ps1             — 一键编译+烧录（PowerShell）
  build4.bat            — 仅编译（CMD.exe 备选方案）
  sdkconfig.defaults    — 默认配置
  sdkconfig.defaults.esp32s3 — ESP32-S3 专用配置
  partitions_dlna_example.csv — 分区表
```

## MiPlay 组件说明

MiPlay 是小米妙播协议，实现在 `components/miplay/miplay.c`。当前状态：

- ✅ mDNS 服务注册（`_miplay_lan._tcp` 端口 8899，subtype=television）
- ✅ 9 字节大端序命令帧编解码
- ✅ 握手流程：Version(0x0036) → VersionAck(0x0037) + Challenge(0x0028) → SafetyAck(0x0029) → SafetyInfo(0x1400) → SafetyInfoAck(0x1401) → SafetyAuth(0x1402) → SafetyAuthAck(0x1403)
- ✅ SafetyData v1：AES-128-CBC + zero padding + CRC-32/MPEG-2
- ✅ 密钥派生：authKey = MD5(IP+端口，数字→字母) 取后半段做 AES key/IV
- ⏳ 音频流传输（RTSP）待实现

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

### MSYSTEM 报错
```
ERROR: MSYSTEM is set to MINGW64. Please use a plain Windows command prompt.
```
→ 不要用 Git Bash。用 PowerShell 或 CMD。或手动 `set MSYSTEM=` 清空。

### Python 版本不匹配
```
'E:\ESP\.espressif\python_env\...' is currently active while project was configured with 'E:\ESP\.espressif\tools\python_env\...'
```
→ 确保 `IDF_PYTHON_ENV_PATH` 设为 `E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env`（注意有 `tools\`）。

### 中文乱码
→ `esp_env.ps1` 已设置 `$env:PYTHONUTF8 = "1"`，如果仍有问题检查终端编码为 UTF-8。
→ Python 串口读取时用 `sys.stdout.reconfigure(errors='replace')` 防止 GBK 编码错误。
