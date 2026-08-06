@echo off
set MSYSTEM=
set IDF_TOOLS_PATH=E:\ESP\.espressif\tools
set IDF_PATH=E:\ESP\.espressif\esp-idf-master
set PYTHONUTF8=1
echo Starting Python env install...
E:\ESP\.espressif\tools\python\v5.5.4\venv\Scripts\python.exe E:\ESP\.espressif\esp-idf-master\tools\idf_tools.py install-python-env > E:\ESP\dlna\s31_install_log.txt 2>&1
echo Done. Exit code: %ERRORLEVEL%
