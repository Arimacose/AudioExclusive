[CmdletBinding()]
param([string]$AppleMusicPackageDirectory='', [string]$CMakePath='cmake', [string]$PythonPath='python')
$ErrorActionPreference='Stop'
if(-not $AppleMusicPackageDirectory){
    $package=Get-AppxPackage AppleInc.AppleMusicWin|Sort-Object Version -Descending|Select-Object -First 1
    if(-not $package){throw 'Install a supported Apple Music client or specify -AppleMusicPackageDirectory'}
    $AppleMusicPackageDirectory=$package.InstallLocation
}
function Invoke-BuildStep([string[]]$Arguments){
    & $CMakePath @Arguments
    if($LASTEXITCODE -ne 0){throw "CMake failed with exit code $LASTEXITCODE"}
}
$build=Join-Path $PSScriptRoot 'build'
$apple=Join-Path $build 'apple'
$netease=Join-Path $build 'netease'
$setup=Join-Path $build 'setup'
Invoke-BuildStep -Arguments @('-S',"$PSScriptRoot/backend-source/apple-source/mod",'-B',$apple,'-A','x64',"-DAPPLE_MUSIC_PACKAGE_DIR=$AppleMusicPackageDirectory")
Invoke-BuildStep -Arguments @('--build',$apple,'--config','Release','--target','am_exclusive_hook','am_exclusive_ui','am_exclusive_broker','am_exclusive_media_control','--parallel','4')
Invoke-BuildStep -Arguments @('-S',"$PSScriptRoot/backend-source/netease-source",'-B',$netease,'-A','x64')
Invoke-BuildStep -Arguments @('--build',$netease,'--config','Release','--parallel','4')
$payload=Join-Path $PSScriptRoot 'installer/generated/payload'
New-Item -ItemType Directory -Path "$payload/AppleMusic","$payload/NetEase" -Force|Out-Null
Copy-Item -LiteralPath "$PSScriptRoot/installer/bundle-template.json" -Destination "$payload/bundle.json" -Force
foreach($file in @('am-exclusive-hook.dll','am-exclusive-ui.dll','am-exclusive-broker.exe','am_exclusive_media_control.exe')){
    Copy-Item -LiteralPath "$apple/Release/$file" -Destination "$payload/AppleMusic/$file" -Force
}
foreach($file in @('nc-exclusive-hook.dll','nc-exclusive-launcher.exe','nc-exclusive-probe.exe','nc-exclusive-asio-probe.exe')){
    Copy-Item -LiteralPath "$netease/Release/$file" -Destination "$payload/NetEase/$file" -Force
}
foreach($file in @('LICENSE','THIRD_PARTY_NOTICES.md')){
    Copy-Item -LiteralPath "$PSScriptRoot/backend-source/apple-source/$file" -Destination "$payload/AppleMusic/$file" -Force
}
Copy-Item -LiteralPath "$PSScriptRoot/installer/Register-AppleCompanion.ps1" -Destination "$payload/AppleMusic/Register-Companion.ps1" -Force
$licenses="$PSScriptRoot/backend-source/netease-source/licenses"
Copy-Item -LiteralPath $licenses -Destination "$payload/NetEase" -Recurse -Force
foreach($file in @('GPL-3.0.txt','ASIO-SDK-LICENSE.txt')){Copy-Item -LiteralPath "$licenses/$file" -Destination "$payload/NetEase/$file" -Force}
Copy-Item -LiteralPath "$PSScriptRoot/backend-source/netease-source/THIRD_PARTY_NOTICES.md" -Destination "$payload/NetEase" -Force
& $PythonPath "$PSScriptRoot/installer/Prepare-Payload.py"
if($LASTEXITCODE -ne 0){throw 'Preparing installer payload failed'}
Invoke-BuildStep -Arguments @('-S',"$PSScriptRoot/installer",'-B',$setup,'-A','x64')
Invoke-BuildStep -Arguments @('--build',$setup,'--config','Release','--parallel','4')
Write-Host "Installer: $setup/Release/AudioExclusive-Setup.exe"
