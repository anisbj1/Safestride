[CmdletBinding()]
param(
    # Install Python packages but do not download PlatformIO toolchains or run all tests.
    [switch]$SkipVerification
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $repoRoot

function Test-PythonVersion {
    param([string]$Executable, [string[]]$PrefixArgs = @())
    try {
        & $Executable @PrefixArgs -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 11) else 1)' *> $null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    }
}

$pythonCommand = $null
$pythonPrefixArgs = @()
$pyLauncher = Get-Command py -ErrorAction SilentlyContinue
$python = Get-Command python -ErrorAction SilentlyContinue

if ($pyLauncher -and (Test-PythonVersion -Executable $pyLauncher.Source -PrefixArgs @('-3'))) {
    $pythonCommand = $pyLauncher.Source
    $pythonPrefixArgs = @('-3')
} elseif ($python -and (Test-PythonVersion -Executable $python.Source)) {
    $pythonCommand = $python.Source
} else {
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if (-not $winget) {
        throw 'Python 3.11+ is required. Install it from https://www.python.org/downloads/ and rerun this script.'
    }
    Write-Host 'Python 3.11+ not found; installing Python 3.12 for this Windows user via winget.'
    & $winget.Source install --id Python.Python.3.12 --exact --scope user `
        --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        throw 'Python installation failed. Install Python 3.11+ and rerun this script.'
    }
    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($pyLauncher -and (Test-PythonVersion -Executable $pyLauncher.Source -PrefixArgs @('-3.12'))) {
        $pythonCommand = $pyLauncher.Source
        $pythonPrefixArgs = @('-3.12')
    } else {
        throw 'Python was installed, but this shell cannot find it yet. Open a new PowerShell window and rerun scripts/setup.ps1.'
    }
}

Write-Host 'Creating/updating the project-local .venv ...'
$venvPath = Join-Path $repoRoot '.venv'
if (-not (Test-Path $venvPath)) {
    & $pythonCommand @pythonPrefixArgs -m venv $venvPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not create .venv.' }
}
$venvPython = Join-Path $venvPath 'Scripts\python.exe'
$venvScripts = Join-Path $venvPath 'Scripts'
if (-not (Test-Path $venvPython)) { throw 'The local .venv is incomplete; remove it and rerun setup.' }
$env:PATH = "$venvScripts;$env:PATH"

Write-Host 'Installing Python development tools ...'
& $venvPython -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) { throw 'pip upgrade failed.' }
& $venvPython -m pip install -r (Join-Path $repoRoot 'requirements-dev.txt')
if ($LASTEXITCODE -ne 0) { throw 'Installing requirements-dev.txt failed.' }

Write-Host 'Installing the local companion package and its BLE dependency ...'
& $venvPython -m pip install -e (Join-Path $repoRoot 'companion')
if ($LASTEXITCODE -ne 0) { throw 'Installing the companion package failed.' }

if ($SkipVerification) {
    Write-Host 'Setup complete. PlatformIO downloads the ESP32 framework/libraries on the first build.'
    Write-Host 'Run the complete verification later with: .\.venv\Scripts\python.exe ci\check.py'
    exit 0
}

Write-Host 'Running the complete build and test suite. The first run downloads PlatformIO toolchains/libraries ...'
& $venvPython (Join-Path $repoRoot 'ci\check.py')
if ($LASTEXITCODE -ne 0) { throw 'Setup finished, but verification failed. See the check output above.' }
Write-Host 'Setup and verification completed successfully.'
