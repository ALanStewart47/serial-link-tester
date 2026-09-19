# Pack a portable Windows Release of serial_test_tool v0.1.
# Usage (from this directory):
#   powershell -ExecutionPolicy Bypass -File .\pack_release.ps1
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Qt = "D:\QT\6.5.3\mingw_64"
$MingwBin = "D:\QT\Tools\mingw1120_64\bin"
$Ninja = "D:\QT\Tools\Ninja"
$CMake = "D:\QT\Tools\CMake_64\bin"
$env:PATH = "$MingwBin;$Ninja;$CMake;$Qt\bin;$env:PATH"

$Version = "v0.1"
$Build = Join-Path $Root "build-release"
$DistRoot = Join-Path (Split-Path $Root -Parent) "dist"
$Dist = Join-Path $DistRoot "serial_test_tool"
$DistVer = Join-Path $DistRoot "serial_test_tool_$Version"
$Zip = Join-Path $DistRoot "serial_test_tool_${Version}_win64.zip"
$ReadmeName = ([string][char]0x4F7F) + [char]0x7528 + [char]0x8BF4 + [char]0x660E + ".txt"

Write-Host "Configuring Release..."
if (Test-Path $Build) {
    Remove-Item -Recurse -Force $Build
}
cmake -S $Root -B $Build -G Ninja "-DCMAKE_PREFIX_PATH=$Qt" "-DCMAKE_CXX_COMPILER=$MingwBin\g++.exe" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "Building..."
cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$Exe = Join-Path $Build "serial_test_tool.exe"
if (-not (Test-Path $Exe)) { throw "serial_test_tool.exe not found: $Exe" }

if (Test-Path $DistRoot) {
    Remove-Item -Recurse -Force $DistRoot
}
New-Item -ItemType Directory -Force $Dist | Out-Null
Copy-Item $Exe $Dist

$ReadmeSrc = Join-Path $Root $ReadmeName
if (-not (Test-Path $ReadmeSrc)) { throw "readme not found: $ReadmeSrc" }
$ReadmeText = [System.IO.File]::ReadAllText($ReadmeSrc, [System.Text.UTF8Encoding]::new($false))
$ReadmeText = $ReadmeText -replace "`r`n", "`n" -replace "`n", "`r`n"
$Gbk = [System.Text.Encoding]::GetEncoding(936)
[System.IO.File]::WriteAllText((Join-Path $Dist $ReadmeName), $ReadmeText, $Gbk)

Write-Host "Running windeployqt..."
# MinGW Qt plugins have no "d" suffix. Do NOT pass --release or qwindows is skipped.
& (Join-Path $Qt "bin\windeployqt.exe") --compiler-runtime --no-translations (Join-Path $Dist "serial_test_tool.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path $Dist -DestinationPath $Zip
if (Test-Path $DistVer) { Remove-Item -Recurse -Force $DistVer }
Copy-Item -Recurse $Dist $DistVer

Write-Host "OK"
Write-Host "  folder: $Dist"
Write-Host "  folder: $DistVer"
Write-Host "  zip:    $Zip"
