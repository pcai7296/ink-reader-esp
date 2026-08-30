# flash.ps1 — 清理占用 + 编译 + 自动烧录（一步完成）
# 流程:
#   1. 查找并终止占用串口/构建文件的进程（esptool/python/其他监听实例）
#   2. arduino-cli compile（--build-path build_cfs）
#   3. esptool write-flash（app@0x0 [+可选 littlefs@0x200000]，默认写入后自动验证）
#   4. 完成后可选重启常驻串口监听
# 用法:
#   pwsh -File flash.ps1                      # 编译+烧 app（无 fs 分区）
#   pwsh -File flash.ps1 -WithFs              # 编译+烧 app + littlefs
#   pwsh -File flash.ps1 -SkipCompile         # 只烧现有 bin（不编译）
#   pwsh -File flash.ps1 -Port COM4 -Baud 460800
param(
  [string]$Port = "COM4",
  [int]$Baud = 460800,
  [switch]$WithFs,
  [switch]$SkipCompile,
  [switch]$StartListen   # 烧录后重启常驻监听（log_N+1）
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$esptool = "C:\Users\Administrator\AppData\Local\Programs\Python\Python312\Scripts\esptool.exe"
$buildDir = Join-Path $root "build_cfs"
$appBin = Join-Path $buildDir "file_manager.ino.bin"
$fsBin = Join-Path $buildDir "file_manager.littlefs.bin"

Write-Host "=== FLASH SCRIPT ==="
$flagWithFs = if ($WithFs) { "yes" } else { "no" }
$flagSkip = if ($SkipCompile) { "yes" } else { "no" }
Write-Host ("target=$Port @$Baud baud=$Baud withFs=$flagWithFs skipCompile=$flagSkip")

# ---- 1. 清理占用 ----
Write-Host "--- [1/3] cleaning port/process occupancy ---"
$killed = @()
# 杀 esptool/python 残留
Get-Process -ErrorAction SilentlyContinue | Where-Object {
  $_.ProcessName -match 'python|esptool' -and $_.Id -ne $PID
} | ForEach-Object {
  try { Stop-Process -Id $_.Id -Force -ErrorAction Stop; $killed += $_.ProcessName } catch {}
}
# 杀其他 serial_listen 监听实例（python 版）
Get-CimInstance Win32_Process -Filter "Name='python.exe' or Name='pythonw.exe'" -ErrorAction SilentlyContinue |
  Where-Object { $_.CommandLine -match 'serial_listen' -and $_.ProcessId -ne $PID } | ForEach-Object {
    try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop; $killed += "listener($($_.ProcessId))" } catch {}
  }
if ($killed.Count -gt 0) { Write-Host ("killed: " + ($killed -join ', ')) } else { Write-Host "nothing to clean" }
Start-Sleep -Seconds 2   # 等端口句柄释放

# ---- 2. 编译 ----
if (-not $SkipCompile) {
  Write-Host "--- [2/3] compiling ---"
  arduino-cli compile --fqbn esp8266:esp8266:d1_mini --libraries (Join-Path $root "libraries") --build-path $buildDir (Join-Path $root "file_manager.ino")
  if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED"; exit 1 }
  Write-Host ("compiled: {0} ({1}B)" -f $appBin, (Get-Item $appBin).Length)
} else {
  Write-Host "--- [2/3] compile skipped ---"
}
if (-not (Test-Path $appBin)) { Write-Host "ERROR: app bin missing: $appBin"; exit 1 }

# ---- 3. 烧录 ----
Write-Host "--- [3/3] flashing ---"
$args = @("--port", $Port, "--baud", "$Baud", "write-flash", "0x0", $appBin)
if ($WithFs) {
  if (-not (Test-Path $fsBin)) { Write-Host "ERROR: fs bin missing: $fsBin"; exit 1 }
  $args += "0x200000", $fsBin
}
& $esptool @args
if ($LASTEXITCODE -ne 0) { Write-Host "FLASH FAILED"; exit 1 }
Write-Host "=== FLASH OK ==="

# ---- 4. 可选重启监听 ----
if ($StartListen) {
  Write-Host "--- restarting serial listener ---"
  Start-Process python -ArgumentList (Join-Path $root "serial_listen.py"), $Port, "115200" -WindowStyle Hidden
  Write-Host "listener restarted (new log_N.txt)"
}
