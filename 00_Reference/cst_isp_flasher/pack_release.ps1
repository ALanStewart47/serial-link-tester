# Pack a portable Windows Release of isp_flasher (exe + Qt/MinGW runtime, no source).
# Usage (from this directory):
#   powershell -ExecutionPolicy Bypass -File .\pack_release.ps1
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Qt = "D:\Qt\6.5.3\mingw_64"
$MingwBin = "D:\Qt\Tools\mingw1120_64\bin"
$Ninja = "D:\Qt\Tools\Ninja"
$CMake = "D:\Qt\Tools\CMake_64\bin"
$env:PATH = "$MingwBin;$Ninja;$CMake;$Qt\bin;$env:PATH"

$Version = "v1.2"
$Build = Join-Path $Root "build-release"
$ReleaseDir = Join-Path $Root "release"
$Dist = Join-Path $ReleaseDir "isp_flasher"
$DistVer = Join-Path $ReleaseDir "isp_flasher-$Version"
$Zip = Join-Path $ReleaseDir "isp_flasher-win64.zip"
$ZipVer = Join-Path $ReleaseDir "isp_flasher-$Version-win64.zip"

Write-Host "Configuring Release..."
cmake -S $Root -B $Build -G Ninja "-DCMAKE_PREFIX_PATH=$Qt" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "Building..."
cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$Exe = Join-Path $Build "isp_flasher.exe"
if (-not (Test-Path $Exe)) { throw "isp_flasher.exe not found: $Exe" }

if (Test-Path $ReleaseDir) {
    Remove-Item -Recurse -Force $ReleaseDir
}
New-Item -ItemType Directory -Force $Dist | Out-Null
Copy-Item $Exe $Dist

# 中文说明单独放 readme.txt，避免 Windows PowerShell 5.1 按 ANSI 解析本脚本导致乱码。
# 发行包写成 GBK（代码页 936），记事本默认打开就是对的。
$ReadmeSrc = Join-Path $Root "readme.txt"
if (-not (Test-Path $ReadmeSrc)) { throw "readme.txt not found: $ReadmeSrc" }
$ReadmeText = [System.IO.File]::ReadAllText($ReadmeSrc, [System.Text.UTF8Encoding]::new($false))
$ReadmeText = $ReadmeText -replace "`r`n", "`n" -replace "`n", "`r`n"
$Gbk = [System.Text.Encoding]::GetEncoding(936)
[System.IO.File]::WriteAllText((Join-Path $Dist "readme.txt"), $ReadmeText, $Gbk)

Write-Host "Running windeployqt..."
# MinGW 的 Qt 插件没有 d 后缀，windeployqt 常把它们误判成 debug。
# 加 --release 会跳过全部插件，报 Unable to find the platform plugin。
& (Join-Path $Qt "bin\windeployqt.exe") --compiler-runtime --no-translations (Join-Path $Dist "isp_flasher.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path $Dist -DestinationPath $Zip
Copy-Item -Force $Zip $ZipVer
if (Test-Path $DistVer) { Remove-Item -Recurse -Force $DistVer }
Copy-Item -Recurse $Dist $DistVer
Write-Host "OK"
Write-Host "  folder: $Dist"
Write-Host "  folder: $DistVer"
Write-Host "  zip:    $Zip"
Write-Host "  zip:    $ZipVer"
