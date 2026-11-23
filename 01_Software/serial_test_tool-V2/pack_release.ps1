# Build and package without removing previous releases.
param(
    [string]$PackageName = ("serial_test_tool_v0.2.6_" + (Get-Date -Format "yyyyMMdd_HHmmss") + "_win64"),
    [string]$BuildDirectory = "build-release"
)
$ErrorActionPreference = "Stop"
if ($PackageName -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw "PackageName must be a directory name" }
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Qt = "D:\QT\6.5.3\mingw_64"
$MingwBin = "D:\QT\Tools\mingw1120_64\bin"
$env:PATH = "$MingwBin;D:\QT\Tools\Ninja;D:\QT\Tools\CMake_64\bin;$Qt\bin;$env:PATH"
$Build = Join-Path $Root $BuildDirectory
$DistRoot = Join-Path (Split-Path $Root -Parent) "dist"
$Dist = Join-Path $DistRoot $PackageName
$Zip = Join-Path $DistRoot "$PackageName.zip"
if ((Test-Path -LiteralPath $Dist) -or (Test-Path -LiteralPath $Zip)) {
    throw "Release already exists; choose a new PackageName. Previous releases are preserved."
}
cmake -S $Root -B $Build -G Ninja "-DCMAKE_PREFIX_PATH=$Qt" "-DCMAKE_CXX_COMPILER=$MingwBin\g++.exe" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build $Build -j 6
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
# A previously deployed build may lack the offscreen plugin used by tests.
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$Qt\plugins\platforms"
ctest --test-dir $Build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Regression tests failed; no release created" }
New-Item -ItemType Directory -Path $Dist -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $Build "serial_test_tool.exe") -Destination $Dist
$ReadmeName = ([string][char]0x4F7F) + [char]0x7528 + [char]0x8BF4 + [char]0x660E + ".txt"
$ReadmeText = [IO.File]::ReadAllText((Join-Path $Root $ReadmeName), [Text.Encoding]::UTF8)
$ReadmeText = $ReadmeText -replace "`r`n", "`n" -replace "`n", "`r`n"
[IO.File]::WriteAllText((Join-Path $Dist $ReadmeName), $ReadmeText, [Text.Encoding]::GetEncoding(936))
$DocName = ([string][char]0x591A) + [char]0x901A + [char]0x9053 + [char]0x529F + [char]0x80FD + [char]0x8BF4 + [char]0x660E + ".md"
$Doc = Join-Path (Split-Path (Split-Path $Root -Parent) -Parent) "00_Doc/$DocName"
Copy-Item -LiteralPath $Doc -Destination $Dist
# MinGW plugins have no d suffix; --release incorrectly skips qwindows here.
& "$Qt\bin\windeployqt.exe" --compiler-runtime --no-translations (Join-Path $Dist "serial_test_tool.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }
foreach ($Required in @("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6SerialPort.dll", "platforms/qwindows.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")) {
    if (-not (Test-Path -LiteralPath (Join-Path $Dist $Required))) { throw "Missing runtime: $Required" }
}
# Explicit release allowlist: the app, runtime DLLs and the two user guides only.
foreach ($File in Get-ChildItem -LiteralPath $Dist -Recurse -File) {
    $Relative = $File.FullName.Substring($Dist.Length + 1)
    $Allowed = ($Relative -eq "serial_test_tool.exe") -or ($Relative -eq $ReadmeName) -or ($Relative -eq $DocName) -or ($File.Extension -ieq ".dll")
    if (-not $Allowed) { throw "Unexpected file in portable package: $Relative" }
}
Compress-Archive -LiteralPath $Dist -DestinationPath $Zip
Write-Host "Folder: $Dist"
Write-Host "ZIP: $Zip"
