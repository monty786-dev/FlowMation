<#
 .SYNOPSIS
   Builds FlowBrowser.exe (WebView2) and/or FlowBrowserWine.exe (Wine / Winlator) and writes everything to build.log.

 .DESCRIPTION
   Needs Windows, CMake 3.21+ and Visual Studio 2019/2022 (workload "Desktop development with C++").
   The log file gets a header (date, OS, git commit, CMake, Visual Studio), the full output of every step, the exit
   code of every step, a list of the compiler/linker errors found, and a final SUCCESS or FAILED line. It is written
   even when the build fails, so you can send it to whoever helps you fix the code.

 .EXAMPLE
   powershell -ExecutionPolicy Bypass -File tools/build.ps1                       # both exes, 64-bit
   powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Target wine          # only FlowBrowserWine.exe (no WebView2 SDK needed)
   powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Arch Win32           # 32-bit exes
   powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Log C:\temp\my.log   # another log location
#>
[CmdletBinding()]
param(
    [ValidateSet("all", "webview2", "wine")][string]$Target = "all",
    [ValidateSet("x64", "Win32", "ARM64")][string]$Arch = "x64",
    [string]$Config = "Release",
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [string]$Log = "",
    [string]$Generator = "",          # empty = the newest Visual Studio generator; e.g. "Ninja" (run from a VS developer prompt)
    [string]$WebView2Version = "",    # empty = newest stable WebView2 SDK
    [switch]$SkipFetch,               # do not download the WebView2 SDK (already in third_party/webview2)
    [switch]$Clean                    # delete the build folder first
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build" }
if (-not $OutDir) { $OutDir = Join-Path $root ("dist\" + $Arch) }
if (-not $Log) { $Log = Join-Path $root "build.log" }
$Log = [IO.Path]::GetFullPath($Log)
$utf8 = New-Object System.Text.UTF8Encoding($false)
$script:t0 = Get-Date

# ---- logging: console + file (UTF-8, no BOM; Tee-Object would write UTF-16 on Windows PowerShell 5.1) ----
function Write-Log([string]$text) {
    Write-Host $text
    try { [IO.File]::AppendAllText($Log, $text + "`r`n", $utf8) } catch { }
}
function Write-LogLines {            # pipeline filter: every line of a native command goes to the console and the log
    process { Write-Log ("" + $_) }
}
function Step([string]$title) {
    Write-Log ""
    Write-Log ("=== " + $title + "  [" + ((Get-Date) - $script:t0).ToString("mm\:ss") + "]")
}
function Run-Native([string]$what, [string]$exe, [string[]]$arguments) {
    Write-Log ("> " + $exe + " " + ($arguments -join " "))
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"       # Windows PowerShell 5.1 turns native stderr into error records
    try {
        & $exe @arguments 2>&1 | Write-LogLines
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    Write-Log ("[" + $what + "] exit code " + $code)
    if ($code -ne 0) { throw ($what + " failed with exit code " + $code) }
}
function Try-Capture([scriptblock]$block) {
    try { $o = & $block 2>$null; if ($o) { return ($o | Out-String).Trim() } } catch { }
    return "n/a"
}

# start a fresh log
[IO.File]::WriteAllText($Log, "", $utf8)
$ok = $false
$outputs = @()
try {
    Write-Log "FlowBrowser build log"
    Write-Log ("Started      : " + (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss") + " UTC")
    Write-Log ("Target       : " + $Target + "   Arch: " + $Arch + "   Config: " + $Config)
    Write-Log ("Source       : " + $root)
    Write-Log ("Build folder : " + $BuildDir)
    Write-Log ("Log file     : " + $Log)
    Write-Log ("Host OS      : " + [Environment]::OSVersion.VersionString + "  (64-bit OS: " + [Environment]::Is64BitOperatingSystem + ")")
    Write-Log ("PowerShell   : " + $PSVersionTable.PSVersion)
    if ($env:GITHUB_ACTIONS) { Write-Log ("CI           : GitHub Actions run " + $env:GITHUB_RUN_ID + " (" + $env:GITHUB_REF + ")") }
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        Write-Log ("Git commit   : " + (Try-Capture { git -C $root rev-parse --short HEAD }) + "   branch: " + (Try-Capture { git -C $root rev-parse --abbrev-ref HEAD }))
        $dirty = Try-Capture { git -C $root status --porcelain }
        if ($dirty -and $dirty -ne "n/a") { Write-Log "Git status   : there are uncommitted changes" }
    } else { Write-Log "Git commit   : git not found" }
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) { throw "cmake was not found. Install CMake 3.21 or newer (https://cmake.org/download/) and open a new terminal." }
    Write-Log ("CMake        : " + ((Try-Capture { cmake --version }) -split "`n")[0])
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        Write-Log ("Visual Studio: " + (Try-Capture { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName }) + " " + (Try-Capture { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion }))
    } else { Write-Log "Visual Studio: vswhere not found (fine if you use -Generator Ninja from a developer prompt)" }

    if ($Clean -and (Test-Path $BuildDir)) {
        Step "Cleaning the build folder"
        Remove-Item $BuildDir -Recurse -Force
    }

    # ---- 1. WebView2 SDK (only the WebView2 exe needs it) ----
    $needSdk = ($Target -eq "all" -or $Target -eq "webview2")
    if ($needSdk) {
        $hdr = Join-Path $root "third_party\webview2\build\native\include\WebView2.h"
        if ($SkipFetch -or (Test-Path $hdr)) {
            Step "WebView2 SDK"
            if (Test-Path $hdr) { Write-Log "already present, not downloading again (delete third_party/webview2 to refresh it)" }
            else { throw "-SkipFetch was given but third_party/webview2 is missing" }
        } else {
            Step "Downloading the WebView2 SDK"
            $fetch = Join-Path $PSScriptRoot "fetch-webview2.ps1"
            if ($WebView2Version) { & $fetch -Version $WebView2Version *>&1 | Write-LogLines } else { & $fetch *>&1 | Write-LogLines }
        }
    } else {
        Step "WebView2 SDK"
        Write-Log "not needed for the Wine / Winlator build"
    }

    # ---- 2. configure ----
    Step "Configuring (CMake)"
    $wv = if ($needSdk) { "ON" } else { "OFF" }
    $wn = if ($Target -eq "all" -or $Target -eq "wine") { "ON" } else { "OFF" }
    $cfgArgs = @("-S", $root, "-B", $BuildDir, "-DFB_BUILD_WEBVIEW2=$wv", "-DFB_BUILD_WINE=$wn")
    if ($Generator) { $cfgArgs += @("-G", $Generator, "-DCMAKE_BUILD_TYPE=$Config") } else { $cfgArgs += @("-A", $Arch) }
    Run-Native "configure" "cmake" $cfgArgs

    # ---- 3. build ----
    Step "Compiling and linking"
    Run-Native "build" "cmake" @("--build", $BuildDir, "--config", $Config, "--parallel")

    # ---- 4. collect the exe files ----
    Step "Results"
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $want = @()
    if ($wv -eq "ON") { $want += "FlowBrowser.exe" }
    if ($wn -eq "ON") { $want += "FlowBrowserWine.exe" }
    foreach ($n in $want) {
        $f = Get-ChildItem -Path $BuildDir -Recurse -Filter $n -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $f) { throw ($n + " was not produced - look for 'error' lines above") }
        Copy-Item $f.FullName (Join-Path $OutDir $n) -Force
        $hash = (Get-FileHash -Algorithm SHA256 -Path $f.FullName).Hash
        Write-Log ("{0,-22} {1,10:N0} bytes   SHA256 {2}" -f $n, $f.Length, $hash)
        $outputs += (Join-Path $OutDir $n)
    }
    Write-Log ("Copied to    : " + $OutDir)
    $ok = $true
}
catch {
    Write-Log ""
    Write-Log ("ERROR: " + $_.Exception.Message)
    if ($_.ScriptStackTrace) { Write-Log $_.ScriptStackTrace }
}
finally {
    # list the compiler / linker / CMake errors so they are easy to find (and easy to send back)
    try {
        $lines = [IO.File]::ReadAllLines($Log)
        $errs = @($lines | Where-Object { $_ -match "error C\d+|error LNK\d+|fatal error|CMake Error|: error " } | Select-Object -Unique)
        $warns = @($lines | Where-Object { $_ -match "warning C\d+" } | Select-Object -Unique)
        Write-Log ""
        Write-Log ("=== Summary: " + $errs.Count + " distinct error line(s), " + $warns.Count + " distinct warning line(s)")
        $errs | Select-Object -First 40 | ForEach-Object { Write-Log ("  " + $_.Trim()) }
        if ($errs.Count -gt 40) { Write-Log ("  ... and " + ($errs.Count - 40) + " more, see above") }
    } catch { }
    $secs = [int]((Get-Date) - $script:t0).TotalSeconds
    Write-Log ""
    if ($ok) { Write-Log ("BUILD SUCCESS in " + $secs + " s") } else { Write-Log ("BUILD FAILED after " + $secs + " s - send this file (" + $Log + ") to whoever helps you with the code") }
    # keep a copy next to the exes and show the result on the GitHub job page
    try { if ($ok -and (Test-Path $OutDir)) { Copy-Item $Log (Join-Path $OutDir "build.log") -Force } } catch { }
    if ($env:GITHUB_STEP_SUMMARY) {
        try {
            $tail = [IO.File]::ReadAllLines($Log) | Select-Object -Last 45
            $md = "### FlowBrowser build ($Arch, $Target): " + $(if ($ok) { "success" } else { "FAILED" }) + "`r`n`r`n" + '```' + "`r`n" + ($tail -join "`r`n") + "`r`n" + '```' + "`r`n"
            [IO.File]::AppendAllText($env:GITHUB_STEP_SUMMARY, $md, $utf8)
        } catch { }
    }
}
if (-not $ok) { exit 1 }
exit 0
