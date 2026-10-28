param(
    [string]$PythonExe = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
$Workspace = Join-Path $ProjectRoot "zephyrproject"
$VenvPython = Join-Path $ProjectRoot ".venv\Scripts\python.exe"

if (-not $PythonExe) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) {
        $PythonExe = $pythonCommand.Source
    } else {
        $bundledPython = "C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
        if (Test-Path $bundledPython) {
            $PythonExe = $bundledPython
        } else {
            throw "Python 3 was not found. Pass -PythonExe with python.exe."
        }
    }
}

if (-not (Test-Path $VenvPython)) {
    & $PythonExe -m venv (Join-Path $ProjectRoot ".venv")
}

& $VenvPython -m pip install --disable-pip-version-check -r (Join-Path $ProjectRoot "updater\requirements.txt")
& $VenvPython -m pip install --disable-pip-version-check west

$West = Join-Path $ProjectRoot ".venv\Scripts\west.exe"
if (-not (Test-Path (Join-Path $Workspace "zephyr\.git"))) {
    & $West init -m https://github.com/zephyrproject-rtos/zephyr.git --mr v4.4.2 $Workspace
    if ($LASTEXITCODE -ne 0) {
        throw "west init failed."
    }
}

Push-Location $Workspace
try {
    & $West update cmsis cmsis_6 hal_stm32
    if ($LASTEXITCODE -ne 0) {
        throw "west update failed."
    }

    & $VenvPython -m pip install --disable-pip-version-check `
        -r (Join-Path $Workspace "zephyr\scripts\requirements-base.txt")
    if ($LASTEXITCODE -ne 0) {
        throw "Zephyr Python requirements failed."
    }
}
finally {
    Pop-Location
}

Write-Host "Zephyr workspace is ready."

