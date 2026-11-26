<#
 Downloads the Microsoft.Web.WebView2 NuGet package (headers + static loader) into third_party/webview2.
 No NuGet client is needed. Usage:
     powershell -ExecutionPolicy Bypass -File tools/fetch-webview2.ps1            # newest stable SDK
     powershell -ExecutionPolicy Bypass -File tools/fetch-webview2.ps1 -Version 1.0.2792.45
#>
param([string]$Version = "", [string]$Dest = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Dest) { $Dest = Join-Path $root "third_party\webview2" }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $Version) {
    $idx = Invoke-RestMethod "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/index.json"
    $stable = $idx.versions | Where-Object { $_ -notmatch "-" }
    $Version = $stable[-1]
}
Write-Host "WebView2 SDK $Version"
$pkg = Join-Path $env:TEMP "webview2-$Version.zip"
Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg" -OutFile $pkg
if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Expand-Archive -Path $pkg -DestinationPath $Dest -Force
Remove-Item $pkg
$h = Join-Path $Dest "build\native\include\WebView2.h"
if (-not (Test-Path $h)) { throw "WebView2.h not found after extracting the package" }
Write-Host "OK: $Dest"
