[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$TestDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Manage-AudioExclusive.ps1') -LibraryOnly
if(Test-Path -LiteralPath $TestDirectory){throw 'Use a fresh test directory'}
$script:ManagerRoot=[IO.Path]::GetFullPath($TestDirectory)
$script:StateRoot=Join-Path $script:ManagerRoot 'state'
$script:BackupRoot=Join-Path $script:ManagerRoot 'backups'
$old=Join-Path $script:ManagerRoot 'packages\old'
$new=Join-Path $script:ManagerRoot 'packages\new'
foreach($entry in @(@($old,'old'),@($new,'new'))){
    Write-Json (Join-Path $entry[0] 'bundle.json') @{build=$entry[1];files=@();applications=@{}}
}
Save-State NetEase @{enabled=$true;package=$old}
Save-State AppleMusic @{enabled=$true;package=$new}
Write-Json (Join-Path $script:ManagerRoot 'package.json') @{path=$new;sha256='fixture'}
$script:SourceRoot=$new
$script:Meta=Read-Json (Join-Path $new 'bundle.json')
function Launch-ApplicationCore([string]$Name,$Found){return [pscustomobject]@{app=$Name;build=$script:Meta.build;package=$script:SourceRoot}}
$netease=Launch-Application NetEase
if($netease.build -ne 'old' -or $netease.package -ne $old){throw 'Apple-only upgrade changed the NetEase launch package'}
if($script:SourceRoot -ne $new -or $script:Meta.build -ne 'new'){throw 'Temporary app context overwrote manager context'}
$apple=Launch-Application AppleMusic
if($apple.build -ne 'new'){throw 'Apple launch selected the old package'}
if((Read-Json (Join-Path $script:StateRoot 'NetEase.json')).package -ne $old){throw 'Unrelated app package state was rewritten'}
'PASS: per-app installed packages survive an Apple-only upgrade; manager context restored'
