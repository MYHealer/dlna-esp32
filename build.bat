@echo off
set "MSYS="
set "MSYSTEM="
set "MSYSCON="
set "IDF_TOOLS_PATH=E:\ESP\.espressif\tools"
set "IDF_PATH=E:\ESP\.espressif\v5.5.4\esp-idf"
set "ADF_PATH=E:\ESP\.espressif\v5.5.4\esp-adf"
set "IDF_PYTHON_ENV_PATH=E:\ESP\.espressif\tools\python\v5.5.4\venv"
set "PATH=E:\ESP\.espressif\tools\cmake\3.30.2\bin;E:\ESP\.espressif\tools\ninja\1.12.1;E:\ESP\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;E:\ESP\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;E:\ESP\.espressif\tools\python\v5.5.4\venv\Scripts;%PATH%"

cd /d "%~dp0"
if %ERRORLEVEL% neq 0 (
    echo FAILED: cannot cd to %~dp0
    pause
    exit /b 1
)

echo === fullclean ===
E:\ESP\.espressif\tools\python\v5.5.4\venv\Scripts\python.exe "%IDF_PATH%\tools\idf.py" fullclean
if %ERRORLEVEL% neq 0 (
    echo fullclean FAILED
    pause
    exit /b 1
)

echo === build ===
E:\ESP\.espressif\tools\python\v5.5.4\venv\Scripts\python.exe "%IDF_PATH%\tools\idf.py" build
if %ERRORLEVEL% neq 0 (
    echo build FAILED
    pause
    exit /b 1
)

echo === BUILD SUCCESS ===
echo.
pause