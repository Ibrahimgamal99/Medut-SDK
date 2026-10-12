# Build the SDK (if needed) and install the Python package on Windows.
# Run from the repo root or from bindings/python/.
#
# Prerequisites: Visual Studio 2022, vcpkg (VCPKG_ROOT set), Python >=3.9
param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root      = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$DistDir   = Join-Path $Root "dist\windows\x64"
$DllSrc    = Join-Path $DistDir "medut.dll"
$DllDst    = Join-Path $ScriptDir "Medut\medut.dll"

# ── 1. Build the SDK if the DLL is missing ───────────────────────────────────
if (-not (Test-Path $DllSrc)) {
    Write-Host "==> DLL not found -- building SDK first..."
    & "$Root\scripts\build-windows.ps1" -VcpkgRoot $VcpkgRoot
}

# ── 2. Copy DLL into the package directory ───────────────────────────────────
Write-Host "==> Copying medut.dll into package..."
Copy-Item $DllSrc $DllDst -Force
Remove-Item -Force "$ScriptDir\medut\medut.so" -ErrorAction SilentlyContinue
Remove-Item -Force "$ScriptDir\medut\medut.dylib" -ErrorAction SilentlyContinue

# ── 3. Uninstall existing package, then install fresh ────────────────────────
Write-Host "==> Uninstalling existing package..."
pip uninstall -y medut 2>$null
Write-Host "==> Installing Python package..."
pip install -e $ScriptDir

# ── 4. Build wheel and retag for PyPI ────────────────────────────────────────
Write-Host "==> Building wheel..."
pip install --quiet setuptools wheel
Push-Location $ScriptDir
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force dist | Out-Null
Remove-Item -Force dist\medut-*-win_amd64.whl -ErrorAction SilentlyContinue
python setup.py bdist_wheel --quiet

Write-Host "==> Retagging wheel to win_amd64..."
python -m wheel tags --platform-tag win_amd64 dist\medut-*-win_amd64.whl --remove
Pop-Location

$Whl = Get-ChildItem "$ScriptDir\dist\medut-*-win_amd64.whl" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $Whl) {
    Write-Error "Wheel build failed -- no .whl found in $ScriptDir\dist"
    exit 1
}

Write-Host ""
Write-Host "Done. Wheel ready at:"
Write-Host "  $($Whl.FullName)"
Write-Host ""
Write-Host "To upload to PyPI:"
Write-Host "  twine upload $($Whl.FullName)"
Write-Host ""
Write-Host "Run an example:"
Write-Host "  python $ScriptDir\examples\quickstart.py account.json"
