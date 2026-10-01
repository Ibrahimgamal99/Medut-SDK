# Clone the third_party sources the build needs, at the revisions below.
# third_party\ is gitignored, so this is how a fresh checkout gets them.
# Keep the pins in sync with scripts/fetch-third-party.sh.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ScriptDir

$Deps = @(
    @{ Name = "re";      Url = "https://github.com/baresip/re.git";      Rev = "fd6c71be2a981f2e961c59c7f1b39b5cbd30494e" }
    @{ Name = "baresip"; Url = "https://github.com/baresip/baresip.git"; Rev = "d5ed26ccff4c884f27061cdf022f18332cda4c9c" }
    @{ Name = "opus";    Url = "https://github.com/xiph/opus.git";       Rev = "503d81b138d76621aae4b12786e90de48aa8db3a" }
)
if ($env:ECHOSDK_TLS -eq "mbedtls") {
    $Deps += @{ Name = "mbedtls"; Url = "https://github.com/Mbed-TLS/mbedtls.git"; Rev = "2f2b202f8e72ef01aa0b743ef9df2abb0a3527d9" }
}

foreach ($dep in $Deps) {
    $dir = Join-Path $Root "third_party\$($dep.Name)"

    # CMake gates on CMakeLists.txt, so that marks an already-usable tree —
    # which must also be at the pinned revision.  Moving it uses checkout -f,
    # which drops the EchoSDK patches the build applied (it re-applies them:
    # the patch steps depend on .git/HEAD) and any hand edits under
    # third_party\.
    if (Test-Path (Join-Path $dir "CMakeLists.txt")) {
        # Windows PowerShell 5.1 turns a native command's stderr into a
        # terminating error under Stop, 2>$null or not — hence the try.
        $have = ""
        try { $have = "$(git -C $dir rev-parse HEAD 2>$null)".Trim() } catch { $have = "" }
        if ($LASTEXITCODE -ne 0) { $have = "" }
        if ($have -eq $dep.Rev) {
            Write-Host "==> $($dep.Name): present"
            continue
        }
        $from = if ($have) { $have.Substring(0, [Math]::Min(12, $have.Length)) } else { "(none)" }
        Write-Host "==> $($dep.Name): $from -> $($dep.Rev.Substring(0,12))"
        git -C $dir fetch -q --depth 1 origin $dep.Rev
        if ($LASTEXITCODE -ne 0) { Write-Error "git fetch failed for $($dep.Name) ($($dep.Rev))"; exit 1 }
        git -C $dir checkout -q -f FETCH_HEAD
        if ($LASTEXITCODE -ne 0) { Write-Error "git checkout failed for $($dep.Name)"; exit 1 }
        continue
    }

    Write-Host "==> $($dep.Name): cloning $($dep.Rev.Substring(0,12)) from $($dep.Url)"
    git init -q $dir
    if ($LASTEXITCODE -ne 0) { Write-Error "git init failed for $($dep.Name)"; exit 1 }

    # A half-finished earlier run can leave origin already set.
    if ((git -C $dir remote) -notcontains "origin") {
        git -C $dir remote add origin $dep.Url
        if ($LASTEXITCODE -ne 0) { Write-Error "git remote add failed for $($dep.Name)"; exit 1 }
    }

    git -C $dir fetch -q --depth 1 origin $dep.Rev
    if ($LASTEXITCODE -ne 0) { Write-Error "git fetch failed for $($dep.Name) ($($dep.Rev))"; exit 1 }

    git -C $dir checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { Write-Error "git checkout failed for $($dep.Name)"; exit 1 }
}

# Always set an exit code: when every dep is already present no native command
# runs, and the caller's Set-StrictMode would fault on an unset $LASTEXITCODE.
exit 0
