$ErrorActionPreference = "Stop"

$ProjectRoot = $PSScriptRoot
$Workspace = Join-Path $ProjectRoot "zephyrproject"
$West = Join-Path $ProjectRoot ".venv\Scripts\west.exe"
$Gcc = Get-Command arm-none-eabi-gcc -ErrorAction Stop
$ToolchainRoot = Split-Path (Split-Path $Gcc.Source -Parent) -Parent
$Dist = Join-Path $ProjectRoot "dist"

$env:ZEPHYR_BASE = Join-Path $Workspace "zephyr"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "gnuarmemb"
$env:GNUARMEMB_TOOLCHAIN_PATH = $ToolchainRoot
New-Item -ItemType Directory -Path $Dist -Force | Out-Null

Push-Location $Workspace
$PreviousDtcOverlay = $env:DTC_OVERLAY_FILE
try {
    & $West build -p always -b stm32_min_dev $ProjectRoot `
        -d (Join-Path $ProjectRoot "build\bootloader")
    if ($LASTEXITCODE -ne 0) {
        throw "Bootloader build failed."
    }

    & $West build -p always -b stm32_min_dev (Join-Path $ProjectRoot "app_template") `
        -d (Join-Path $ProjectRoot "build\app_slot_a")
    if ($LASTEXITCODE -ne 0) {
        throw "Slot A application build failed."
    }

    $env:DTC_OVERLAY_FILE = (Join-Path $ProjectRoot "app_template\slot_b.overlay").Replace(
        "\", "/"
    )
    & $West build -p always -b stm32_min_dev (Join-Path $ProjectRoot "app_template") `
        -d (Join-Path $ProjectRoot "build\app_slot_b")
    if ($LASTEXITCODE -ne 0) {
        throw "Slot B application build failed."
    }
}
finally {
    $env:DTC_OVERLAY_FILE = $PreviousDtcOverlay
    Pop-Location
}

Copy-Item (Join-Path $ProjectRoot "build\bootloader\zephyr\zephyr.bin") `
    (Join-Path $Dist "f103_usb_bootloader.bin") -Force
Copy-Item (Join-Path $ProjectRoot "build\bootloader\zephyr\zephyr.hex") `
    (Join-Path $Dist "f103_usb_bootloader.hex") -Force
Copy-Item (Join-Path $ProjectRoot "build\bootloader\zephyr\zephyr.elf") `
    (Join-Path $Dist "f103_usb_bootloader.elf") -Force
Copy-Item (Join-Path $ProjectRoot "build\app_slot_a\zephyr\zephyr.bin") `
    (Join-Path $Dist "app_template_slot_a.bin") -Force
Copy-Item (Join-Path $ProjectRoot "build\app_slot_a\zephyr\zephyr.hex") `
    (Join-Path $Dist "app_template_slot_a.hex") -Force
Copy-Item (Join-Path $ProjectRoot "build\app_slot_b\zephyr\zephyr.bin") `
    (Join-Path $Dist "app_template_slot_b.bin") -Force
Copy-Item (Join-Path $ProjectRoot "build\app_slot_b\zephyr\zephyr.hex") `
    (Join-Path $Dist "app_template_slot_b.hex") -Force

Write-Host ""
Write-Host "Bootloader:"
Write-Host "  build\bootloader\zephyr\zephyr.bin"
Write-Host "Application slot A:"
Write-Host "  build\app_slot_a\zephyr\zephyr.bin"
Write-Host "Application slot B:"
Write-Host "  build\app_slot_b\zephyr\zephyr.bin"

