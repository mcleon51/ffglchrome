# Downloads the WebView2 SDK from NuGet and extracts the needed files
# into the deps/webview2/ directory structure expected by CMake.

$PackageName = "Microsoft.Web.WebView2"
$Version     = "1.0.2957.106"   # latest stable as of May 2026
$OutputDir   = Join-Path $PSScriptRoot "_cache"
$IncludeDir  = Join-Path $PSScriptRoot "include"
$BinDir      = Join-Path $PSScriptRoot "bin"
$LibDir      = Join-Path $PSScriptRoot "lib"

Write-Host "=== WebView2 SDK downloader ===" -ForegroundColor Cyan
Write-Host "Package : $PackageName $Version"
Write-Host "Target  : $PSScriptRoot"
Write-Host ""

# Ensure target dirs exist
foreach ($d in @($IncludeDir, $BinDir, $LibDir)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}

# Download NuGet package (.nupkg = .zip)
$NupkgUrl  = "https://www.nuget.org/api/v2/package/$PackageName/$Version"
$NupkgFile = Join-Path $OutputDir "$PackageName.$Version.nupkg"

Write-Host "Downloading $NupkgUrl ..." -ForegroundColor Yellow
if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null }

try {
    Invoke-WebRequest -Uri $NupkgUrl -OutFile $NupkgFile -UseBasicParsing -ErrorAction Stop
    Write-Host "Downloaded: $NupkgFile" -ForegroundColor Green
} catch {
    Write-Host "Download FAILED: $_" -ForegroundColor Red
    Write-Host "Download manually from:" -ForegroundColor Yellow
    Write-Host "  $NupkgUrl"
    Write-Host "then re-run this script, or extract build/native/ to deps/webview2/"
    exit 1
}

# Extract using System.IO.Compression.ZipFile (works with .nupkg extension)
$ExtractDir = Join-Path $OutputDir "extracted"
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
    [System.IO.Compression.ZipFile]::ExtractToDirectory($NupkgFile, $ExtractDir)
    Write-Host "Extracted successfully" -ForegroundColor Green
} catch {
    Write-Host "Extraction FAILED: $_" -ForegroundColor Red
    exit 1
}

# Copy headers
Write-Host "Copying headers ..." -ForegroundColor Yellow
$HeaderFiles = @(
    "build/native/include/WebView2.h",
    "build/native/include/WebView2EnvironmentOptions.h"
)
if (Test-Path (Join-Path $ExtractDir "build/native/include/WebView2Interop.h")) {
    $HeaderFiles += "build/native/include/WebView2Interop.h"
}
foreach ($hf in $HeaderFiles) {
    $src = Join-Path $ExtractDir $hf
    $dst = Join-Path $IncludeDir (Split-Path $hf -Leaf)
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination $dst -Force
        Write-Host "  $dst" -ForegroundColor Green
    }
}

# Copy x64 binaries
Write-Host "Copying x64 binaries ..." -ForegroundColor Yellow
$BinFiles = @(
    @{src="build/native/x64/WebView2Loader.dll"; dst=$BinDir},
    @{src="build/native/x64/WebView2Loader.dll.lib"; dst=$LibDir}
)
foreach ($bf in $BinFiles) {
    $src = Join-Path $ExtractDir $bf.src
    $dst = Join-Path $bf.dst (Split-Path $bf.src -Leaf)
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination $dst -Force
        Write-Host "  $dst" -ForegroundColor Green
    }
}

# Clean up
Remove-Item -Path $OutputDir -Recurse -Force

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Cyan
Write-Host "WebView2 SDK ready at $PSScriptRoot"
Write-Host ""
Write-Host "Re-run CMake to build ChromeBrowser.dll" -ForegroundColor Green
