. "E:\ESP\esp_env.ps1"
Set-Location "E:\ESP\dlna"
& $PYTHON $IDF_PY -p COM31 flash
if ($LASTEXITCODE -eq 0) { Write-Host "FLASH SUCCESS!" -ForegroundColor Green }
else { Write-Host "FLASH FAILED!" -ForegroundColor Red; exit 1 }
