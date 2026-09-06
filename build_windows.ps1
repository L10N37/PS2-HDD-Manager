param(
    [string]$QtRoot = "C:\msys64\ucrt64",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [ValidateRange(1, 64)]
    [int]$Jobs = 10,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent -Path $MyInvocation.MyCommand.Definition
$QtBin = Join-Path $QtRoot "bin"
$CMake = Join-Path $QtBin "cmake.exe"
$Ninja = Join-Path $QtBin "ninja.exe"
$DeployQt = Join-Path $QtBin "windeployqt6.exe"

if (-not (Test-Path $CMake)) {
    $CMakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $CMakeCommand) {
        throw "CMake was not found in $QtBin or PATH."
    }
    $CMake = $CMakeCommand.Source
}
if (-not (Test-Path $Ninja)) {
    $NinjaCommand = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if (-not $NinjaCommand) {
        throw "Ninja was not found in $QtBin or PATH."
    }
    $Ninja = $NinjaCommand.Source
}
if (-not (Test-Path $DeployQt)) {
    $DeployQt = Join-Path $QtBin "windeployqt.exe"
}
if (-not (Test-Path $DeployQt)) {
    throw "windeployqt was not found below $QtBin."
}

$env:Path = "$QtBin;$env:Path"

$BuildDirectory = Join-Path $ProjectRoot "build\windows-$($Configuration.ToLowerInvariant())"
if ($Clean -and (Test-Path $BuildDirectory)) {
    Remove-Item -Recurse -Force $BuildDirectory
}

& $CMake -S $ProjectRoot -B $BuildDirectory -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_PREFIX_PATH=$QtRoot" `
    "-DPS2_HDD_BUILD_GUI=ON"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed."
}

& $CMake --build $BuildDirectory --parallel $Jobs
if ($LASTEXITCODE -ne 0) {
    throw "PS2 HDD Manager build failed."
}

& $CMake --build $BuildDirectory --target test
if ($LASTEXITCODE -ne 0) {
    throw "PS2 HDD Manager core tests failed."
}

$Executable = Join-Path $BuildDirectory "PS2-HDD-Manager.exe"
if (-not (Test-Path $Executable)) {
    throw "The expected executable was not produced: $Executable"
}

& $DeployQt --compiler-runtime $Executable
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed."
}

Write-Host ""
Write-Host "PS2 HDD Manager build and tests completed successfully." -ForegroundColor Green
Write-Host $Executable
