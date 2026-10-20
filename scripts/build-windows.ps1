# Build Medut for Windows x64 or arm64 (-Arch; defaults to the host's).
# Output: dist\windows\<arch>\medut.dll + medut.lib (import lib) + bare.lib + include\,
#         and a copy of medut.dll in bindings\flutter\windows\<arch>\ for the Flutter plugin.
#
# Prerequisites:
#   - Visual Studio 2022 (or 2019)
#   - vcpkg with the <arch>-windows-static-md triplet:
#       vcpkg install openssl:x64-windows-static-md zlib:x64-windows-static-md
#     (arm64-windows-static-md for -Arch arm64)
#   - cmake (in PATH)
# Audio: WASAPI (built into Windows — no extra install needed)
# Runtime: MSVC CRT (dynamic, always present on Windows desktop)

param(
    [string]$BuildType = "Release",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [ValidateSet("", "x64", "arm64")]
    [string]$Arch = ""
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

# The machine this runs on, and the one the DLL is for (the host's unless
# -Arch says otherwise). MSVC names its tool directories after both.
$HostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
if (-not $Arch) { $Arch = $HostArch }
$VsPlatform = if ($Arch -eq "arm64") { "ARM64" } else { "x64" }
$Triplet = "$Arch-windows-static-md"
$DistDir = Join-Path $Root "dist\windows\$Arch"
$BuildDir = Join-Path $Root "build\windows-$Arch"

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

Write-Host "=== Configuring Windows $Arch (on $HostArch) ==="
cmake -S $Root -B $BuildDir `
    -G "Visual Studio 17 2022" -A $VsPlatform `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DVCPKG_TARGET_TRIPLET=$Triplet" `
    -DMEDUT_TLS=openssl `
    -DMEDUT_MODULES_PROFILE=desktop `
    -DMEDUT_WITH_WEBRTC_AEC=OFF

Write-Host "=== Building ==="
cmake --build $BuildDir --config $BuildType --target medut
if ($LASTEXITCODE -ne 0) { Write-Error "cmake --build failed (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

Write-Host "=== Installing ==="
cmake --install $BuildDir --config $BuildType
if ($LASTEXITCODE -ne 0) { Write-Error "cmake --install failed (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

$StaticLib = Join-Path $DistDir "bare.lib"
if (Test-Path $StaticLib) {
    $size = (Get-Item $StaticLib).Length / 1MB
    Write-Host ""
    Write-Host "Static lib: $StaticLib ($([math]::Round($size,1)) MB)"
} else {
    Write-Error "Install step succeeded but $StaticLib not found"
    exit 1
}

# ── Link shared library (DLL, no extra runtime deps beyond MSVC CRT) ─────────
# vcpkg <arch>-windows-static-md: OpenSSL + zlib statically embedded.
# opus is already in bare.lib (built from third_party/opus by CMake).
# Windows system libs (ws2_32/crypt32/ole32/avrt etc.) are always present.
$VcpkgLibs  = Join-Path $VcpkgRoot "installed\$Triplet\lib"
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
    Write-Error "OpenSSL library not found at $SslLib. Please install: vcpkg install openssl:$Triplet"
    exit 1
}
if (-not (Test-Path $CryptoLib)) {
    Write-Error "Crypto library not found at $CryptoLib. Please install: vcpkg install openssl:$Triplet"
    exit 1
}
if (-not (Test-Path $ZlibLib)) {
    Write-Error "Zlib library not found at $ZlibLib. Please install: vcpkg install zlib:$Triplet"
    exit 1
}

$DllPath = Join-Path $DistDir "medut.dll"
$DefPath = Join-Path $DistDir "medut.def"
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
    $LinkExe = Join-Path $VsPath "VC\Tools\MSVC\*\bin\Host$HostArch\$Arch\link.exe"
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
    # vcvarsall names a native toolset by its arch, a cross one host_target.
    $VcvarsArch = if ($HostArch -eq $Arch) { $Arch } else { "$($HostArch)_$Arch" }
    $BatContent = "@echo off`ncall `"$VcvarsAll`" $VcvarsArch >nul 2>&1`n`"$LinkExe`" $LinkArgs`n"
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
$PluginDll = Join-Path $Root "bindings\flutter\windows\$Arch\medut.dll"
New-Item -ItemType Directory -Force (Split-Path -Parent $PluginDll) | Out-Null
Copy-Item $DllPath $PluginDll -Force
Write-Host "Staged $PluginDll"

Write-Host ""
Write-Host "Done. Output:"
Get-Item $StaticLib, $DllPath -ErrorAction SilentlyContinue | Select-Object Name, @{N="MB";E={[math]::Round($_.Length/1MB,1)}}
