# Build Medut for Windows x64.
# Output: dist\windows\x64\medut.dll + medut.lib (import lib) + bare.lib + dist\windows\x64\include\,
#         and a copy of medut.dll in bindings\flutter\windows\ for the Flutter plugin.
#
# Prerequisites:
#   - Visual Studio 2022 (or 2019)
#   - vcpkg with x64-windows-static-md triplet:
#       vcpkg install openssl zlib abseil:x64-windows-static-md
#   - cmake (in PATH)
# Audio: WASAPI (built into Windows — no extra install needed)
# Runtime: MSVC CRT (dynamic, always present on Windows desktop)

param(
    [string]$BuildType = "Release",
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Allow running this script without changing system execution policy
if ($PSVersionTable.PSVersion.Major -ge 5) {
    $currentExecutionPolicy = Get-ExecutionPolicy -Scope Process -ErrorAction SilentlyContinue
    if ($currentExecutionPolicy -eq "Restricted") {
        Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process -Force
    }
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $Root "build\windows-x64"

if (-not $VcpkgRoot) {
    $VcpkgRoot = @("C:\vcpkg","D:\vcpkg","$env:USERPROFILE\vcpkg") |
        Where-Object { Test-Path "$_\scripts\buildsystems\vcpkg.cmake" } |
        Select-Object -First 1
}
if (-not $VcpkgRoot) {
    Write-Error "vcpkg not found. Set VCPKG_ROOT or install vcpkg to C:\vcpkg."
    exit 1
}

$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $Toolchain)) {
    Write-Error "vcpkg toolchain not found at $Toolchain"
    exit 1
}

Write-Host "=== Fetching third_party ==="
& (Join-Path $ScriptDir "fetch-third-party.ps1")
if ($LASTEXITCODE -ne 0) { Write-Error "third_party fetch failed (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

Write-Host "=== Configuring Windows x64 ==="
cmake -S $Root -B $BuildDir `
    -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DMEDUT_TLS=openssl `
    -DMEDUT_MODULES_PROFILE=desktop `
    -DMEDUT_WITH_WEBRTC_AEC=OFF

Write-Host "=== Building ==="
cmake --build $BuildDir --config $BuildType --target medut
if ($LASTEXITCODE -ne 0) { Write-Error "cmake --build failed (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

Write-Host "=== Installing ==="
cmake --install $BuildDir --config $BuildType
if ($LASTEXITCODE -ne 0) { Write-Error "cmake --install failed (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

$StaticLib = Join-Path $Root "dist\windows\x64\bare.lib"
if (Test-Path $StaticLib) {
    $size = (Get-Item $StaticLib).Length / 1MB
    Write-Host ""
    Write-Host "Static lib: $StaticLib ($([math]::Round($size,1)) MB)"
} else {
    Write-Error "Install step succeeded but $StaticLib not found"
    exit 1
}

# ── Link shared library (DLL, no extra runtime deps beyond MSVC CRT) ─────────
# vcpkg x64-windows-static-md: OpenSSL + zlib statically embedded.
# opus is already in bare.lib (built from third_party/opus by CMake).
# Windows system libs (ws2_32/crypt32/ole32/avrt etc.) are always present.
$VcpkgLibs  = Join-Path $VcpkgRoot "installed\x64-windows-static-md\lib"
$SslLib     = Join-Path $VcpkgLibs "libssl.lib"
$CryptoLib  = Join-Path $VcpkgLibs "libcrypto.lib"

# Check for zlib - vcpkg names it differently for static builds
$ZlibLib = Join-Path $VcpkgLibs "zlib.lib"
if (-not (Test-Path $ZlibLib)) {
    # Try the static naming convention (zs.lib for static)
    $ZlibLib = Join-Path $VcpkgLibs "zs.lib"
}

# Check if vcpkg libraries exist
if (-not (Test-Path $SslLib)) {
    Write-Error "OpenSSL library not found at $SslLib. Please install: vcpkg install openssl:x64-windows-static-md"
    exit 1
}
if (-not (Test-Path $CryptoLib)) {
    Write-Error "Crypto library not found at $CryptoLib. Please install: vcpkg install openssl:x64-windows-static-md"
    exit 1
}
if (-not (Test-Path $ZlibLib)) {
    Write-Error "Zlib library not found at $ZlibLib. Please install: vcpkg install zlib:x64-windows-static-md"
    exit 1
}

$DllPath = Join-Path $Root "dist\windows\x64\medut.dll"
$DefPath = Join-Path $Root "dist\windows\x64\medut.def"
$HeaderPath = Join-Path $Root "include\medut.h"

Write-Host ""
Write-Host "=== Generating DEF file ==="
$exports = @()
foreach ($line in Get-Content $HeaderPath) {
    if ($line -match '^\s*MEDUT_EXPORT\s+.*?[*\s](\w+)\s*\(') {
        $exports += $Matches[1]
    }
}
"EXPORTS`n" + (($exports | Sort-Object -Unique | ForEach-Object { "    $_" }) -join "`n") + "`n" |
    Out-File -FilePath $DefPath -Encoding ASCII
Write-Host "Generated $DefPath with $($exports.Count) exports"

Write-Host ""
Write-Host "=== Linking $DllPath ==="

# Find link.exe and set up VS environment
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $VsWhere) {
    $VsPath = & $VsWhere -latest -property installationPath
    $LinkExe = Join-Path $VsPath "VC\Tools\MSVC\*\bin\Hostx64\x64\link.exe"
    $LinkExe = (Get-Item $LinkExe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    
    # Find vcvarsall.bat to set up environment
    $VcvarsAll = Join-Path $VsPath "VC\Auxiliary\Build\vcvarsall.bat"
}

if (-not $LinkExe -or -not (Test-Path $LinkExe)) {
    Write-Error "Could not find link.exe. Please run from Visual Studio Developer Command Prompt."
    exit 1
}

# Run link.exe through cmd with vcvarsall environment
$LinkArgs = "/DLL /NOLOGO /DEF:`"$DefPath`" /OUT:`"$DllPath`" /WHOLEARCHIVE:`"$StaticLib`" `"$SslLib`" `"$CryptoLib`" `"$ZlibLib`" ws2_32.lib iphlpapi.lib crypt32.lib secur32.lib bcrypt.lib ole32.lib oleaut32.lib avrt.lib advapi32.lib user32.lib shell32.lib qwave.lib"

if (Test-Path $VcvarsAll) {
    # Use call operator with properly escaped arguments
    $BatContent = "@echo off`ncall `"$VcvarsAll`" x64 >nul 2>&1`n`"$LinkExe`" $LinkArgs`n"
    $TempBat = Join-Path $env:TEMP "link_medut.bat"
    $BatContent | Out-File -FilePath $TempBat -Encoding ASCII
    & cmd /c $TempBat
    $LinkExit = $LASTEXITCODE
    Remove-Item $TempBat -ErrorAction SilentlyContinue
} else {
    # Fallback: try running link.exe directly (may fail if SDK paths not set)
    & $LinkExe /DLL /NOLOGO "/DEF:$DefPath" "/OUT:$DllPath" "/WHOLEARCHIVE:$StaticLib" $SslLib $CryptoLib $ZlibLib ws2_32.lib iphlpapi.lib crypt32.lib secur32.lib bcrypt.lib ole32.lib oleaut32.lib avrt.lib advapi32.lib user32.lib shell32.lib qwave.lib
    $LinkExit = $LASTEXITCODE
}
if ($LinkExit -ne 0 -or -not (Test-Path $DllPath)) {
    Write-Error "link.exe failed (exit $LinkExit)"
    exit 1
}

# ── Stage for the Flutter plugin ─────────────────────────────────────────────
# The plugin's windows\CMakeLists.txt bundles this committed copy into the app.
$PluginDll = Join-Path $Root "bindings\flutter\windows\medut.dll"
Copy-Item $DllPath $PluginDll -Force
Write-Host "Staged $PluginDll"

Write-Host ""
Write-Host "Done. Output:"
Get-Item $StaticLib, $DllPath -ErrorAction SilentlyContinue | Select-Object Name, @{N="MB";E={[math]::Round($_.Length/1MB,1)}}
