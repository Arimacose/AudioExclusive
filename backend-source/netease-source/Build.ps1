[CmdletBinding()]
param([string]$BuildDirectory=(Join-Path $PSScriptRoot '..\build'),[string]$CMakePath='')
$ErrorActionPreference='Stop'
if(-not $CMakePath){
    $taskCMake=Get-Command cmake.exe -ErrorAction SilentlyContinue
    if($taskCMake){$CMakePath=$taskCMake.Source}
    else{$CMakePath='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'}
}
if(-not (Test-Path -LiteralPath $CMakePath)){throw 'CMake was not found; pass -CMakePath explicitly'}
& $CMakePath -S $PSScriptRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw "CMake configure failed: $LASTEXITCODE"}
& $CMakePath --build $BuildDirectory --config Release --parallel 4
if($LASTEXITCODE -ne 0){throw "Build failed: $LASTEXITCODE"}
# This script compiles only. It never runs tests, launches NetEase, or installs.
