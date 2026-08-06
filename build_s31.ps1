# ESP32-S31 编译脚本（IDF master v6.2 + RISC-V）
# 直接调用 cmake + ninja，绕过 idf.py 的 PATH 传递问题

$env:IDF_TOOLS_PATH = "E:\ESP\.espressif\tools"
$env:IDF_PATH = "E:\ESP\.espressif\esp-idf-master"
$env:ADF_PATH = "E:\ESP\.espressif\v5.5.4\esp-adf"
$env:ESP_IDF_VERSION = "6.2.0"
$env:IDF_MAINTAINER = "1"
$env:IDF_PYTHON_ENV_PATH = "E:\ESP\.espressif\tools\python_env\idf6.2_py3.14_env"
$env:PYTHONUTF8 = "1"
$env:MSYSTEM = ""

# IDF master 工具链路径
$riscv_bin = "E:\ESP\.espressif\tools\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin"
$cmake_bin = "E:\ESP\.espressif\tools\cmake\4.0.3\bin"
$ninja_bin = "E:\ESP\.espressif\tools\ninja\1.12.1"
$python_env = "E:\ESP\.espressif\tools\python_env\idf6.2_py3.14_env\Scripts"

$env:PATH = "E:\ESP\.espressif\tools\cmake\4.0.3\bin;" +
    "E:\ESP\.espressif\tools\ninja\1.12.1;" +
    "E:\ESP\.espressif\tools\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin;" +
    "E:\ESP\.espressif\tools\python_env\idf6.2_py3.14_env\Scripts;" +
    "$env:PATH"

# 设置编译器完整路径（避免 cmake find_program 找不到）
$env:CC = "E:\ESP\.espressif\tools\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe"
$env:CXX = "E:\ESP\.espressif\tools\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin\riscv32-esp-elf-g++.exe"
$env:ASM = "E:\ESP\.espressif\tools\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe"

$PYTHON = "E:\ESP\.espressif\tools\python_env\idf6.2_py3.14_env\Scripts\python.exe"
$CMAKE = "E:\ESP\.espressif\tools\cmake\4.0.3\bin\cmake.exe"
$NINJA = "E:\ESP\.espressif\tools\ninja\1.12.1\ninja.exe"

Set-Location "E:\ESP\dlna"

Write-Host "=== ESP32-S31 Build (IDF master v6.2, RISC-V) ===" -ForegroundColor Cyan

# 清理旧 build 目录
if (Test-Path "E:\ESP\dlna\build") {
    Remove-Item -Recurse -Force "E:\ESP\dlna\build"
    Write-Host "  Removed old build directory." -ForegroundColor Gray
}

# cmake 配置
Write-Host "Configuring cmake for esp32s31..." -ForegroundColor Yellow
& $CMAKE -G Ninja -B "E:\ESP\dlna\build" `
    -DPYTHON_DEPS_CHECKED=1 `
    "-DPYTHON=$PYTHON" `
    -DESP_PLATFORM=1 `
    -DIDF_TARGET=esp32s31 `
    -DCCACHE_ENABLE=False `
    -DCONFIGDEP_ENABLE=True `
    "E:\ESP\dlna" 2>&1 | Where-Object { $_ -notmatch '^NOTE:' -and $_ -notmatch '^fatal:' }
if ($LASTEXITCODE -ne 0) {
    Write-Host "cmake configure failed!" -ForegroundColor Red
    exit 1
}
Write-Host "  Configure done." -ForegroundColor Green

# ninja 编译
Write-Host "Building with ninja..." -ForegroundColor Yellow
& $NINJA -C "E:\ESP\dlna\build" 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBUILD SUCCESS!" -ForegroundColor Green
} else {
    Write-Host "`nBUILD FAILED!" -ForegroundColor Red
    exit 1
}
