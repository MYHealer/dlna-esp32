. "E:\ESP\esp_env.ps1"
Set-Location "E:\ESP\dlna"
Write-Host "Monitoring ESP32 serial (30s)..." -ForegroundColor Cyan
& $PYTHON $IDF_PY -p COM31 monitor --print_filter="*:W" 2>&1
