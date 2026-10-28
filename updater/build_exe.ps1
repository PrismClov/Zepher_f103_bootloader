$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Python = Join-Path $ProjectRoot ".venv\Scripts\python.exe"

if (-not (Test-Path $Python)) {
    throw "Run setup.ps1 first."
}

& $Python -m pip install --disable-pip-version-check pyinstaller
if ($LASTEXITCODE -ne 0) {
    throw "Could not install PyInstaller."
}

Push-Location $ProjectRoot
try {
    & $Python -m PyInstaller --noconfirm --clean --onefile `
        --name f103_uploader `
        --distpath (Join-Path $ProjectRoot "dist") `
        --workpath (Join-Path $ProjectRoot "build\pyinstaller") `
        --specpath (Join-Path $ProjectRoot "build\pyinstaller") `
        (Join-Path $PSScriptRoot "f103_uploader.py")
    if ($LASTEXITCODE -ne 0) {
        throw "CLI host updater build failed."
    }

    & $Python -m PyInstaller --noconfirm --clean --onefile --windowed `
        --name F103_USB_Updater `
        --distpath (Join-Path $ProjectRoot "dist") `
        --workpath (Join-Path $ProjectRoot "build\pyinstaller_gui") `
        --specpath (Join-Path $ProjectRoot "build\pyinstaller_gui") `
        (Join-Path $PSScriptRoot "f103_gui.py")
    if ($LASTEXITCODE -ne 0) {
        throw "GUI host updater build failed."
    }
}
finally {
    Pop-Location
}

Write-Host "Host updater: dist\f103_uploader.exe"
Write-Host "GUI host updater: dist\F103_USB_Updater.exe"

