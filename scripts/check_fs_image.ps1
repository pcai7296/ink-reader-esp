# S0 integration test: LittleFS web UI image generation check.
# Red/green: run BEFORE creating data/ -> must FAIL; after data/index.html -> PASS.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\check_fs_image.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$data = Join-Path $root "data"
$img  = Join-Path $root "build\fs.bin"
$mk   = "J:\Arduino15\packages\esp8266\hardware\esp8266\3.1.2\tools\mklittlefs\mklittlefs.exe"

$EXPECT_SIZE = 2072576   # 4M2M FS partition: 0x200000..0x3FA000 (boards.txt spiffs_start/end)

$fail = @()
if (-not (Test-Path $data)) { $fail += "data dir missing: $data" }
elseif (-not (Test-Path (Join-Path $data "index.html"))) { $fail += "data/index.html missing" }
if (Test-Path $img) { Remove-Item $img -Force }

if ($fail.Count -gt 0) {
  Write-Output "FS_IMAGE_TEST FAIL"
  $fail | ForEach-Object { Write-Output "  - $_" }
  exit 1
}

& $mk -c $data -b 8192 -p 256 -s $EXPECT_SIZE $img 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Output "FS_IMAGE_TEST FAIL: mklittlefs exit=$LASTEXITCODE"; exit 1 }
if (-not (Test-Path $img)) { Write-Output "FS_IMAGE_TEST FAIL: fs.bin not generated"; exit 1 }

$size = (Get-Item $img).Length
if ($size -ne $EXPECT_SIZE) { Write-Output "FS_IMAGE_TEST FAIL: fs.bin size $size != $EXPECT_SIZE"; exit 1 }

$list = & $mk -l $img 2>&1
if ($LASTEXITCODE -ne 0 -or ($list -notmatch "index\.html")) {
  Write-Output "FS_IMAGE_TEST FAIL: index.html not listed in image"
  Write-Output $list
  exit 1
}

Write-Output "FS_IMAGE_TEST PASS: fs.bin=$size bytes, index.html in image"
exit 0
