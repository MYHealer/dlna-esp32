. "E:\ESP\esp_env.ps1"

Set-Location "E:\ESP\dlna"
& $PYTHON $IDF_PY build

if ($LASTEXITCODE -eq 0) {
    Write-Host "BUILD SUCCESS!" -ForegroundColor Green
} else {
    Write-Host "BUILD FAILED!" -ForegroundColor Red
    exit 1
}
