param([Parameter(Mandatory=$true)][string]$TestDirectory)
$ErrorActionPreference='Stop'
$base=$PSScriptRoot
. (Join-Path $base 'Manage-AudioExclusive.ps1') -LibraryOnly -App NetEase
$fixture=[IO.Path]::GetFullPath($TestDirectory)
if(Test-Path -LiteralPath $fixture){throw 'Use a fresh test directory'}
$script:ManagerRoot=$fixture
$script:StateRoot=Join-Path $fixture 'state'
$script:BackupRoot=Join-Path $fixture 'backups'
$script:NetEaseInstall=Join-Path $fixture 'apps\NetEase'
$script:AppleInstall=Join-Path $fixture 'apps\AppleMusic'
$package=Join-Path $fixture 'packages\fixture'
New-Item -ItemType Directory -Path $package,$script:NetEaseInstall -Force|Out-Null
Copy-Item -Path "$base\generated\payload\*" -Destination $package -Recurse
Copy-Item -Path "$base\generated\payload\NetEase\*" -Destination $script:NetEaseInstall -Recurse
Save-State 'NetEase' @{enabled=$true;package=$package}
Write-Json (Join-Path $fixture 'package.json') @{path=(Join-Path $fixture 'packages\deleted-old-package');sha256='old'}
$actual=Resolve-InstalledPackageRoot
if($actual -ne $package){throw 'Did not recover installed package'}
if((Read-Json (Join-Path $fixture 'package.json')).path -ne $package){throw 'Reference not saved'}
if(@(Get-ChildItem -LiteralPath $script:BackupRoot -File).Count -ne 1){throw 'Missing original reference backup'}
'PASS: missing package reference recovered and persisted'
if((Resolve-InstalledPackageRoot) -ne $package){throw 'Valid reference changed'}
'PASS: existing valid reference retained'
Write-Json (Join-Path $fixture 'package.json') @{path=(Join-Path $fixture 'packages\deleted-old-package');sha256='old'}
[IO.File]::WriteAllText((Join-Path $script:NetEaseInstall 'nc-exclusive-hook.dll'),'different plugin')
$rejected=$false
try { $null=Resolve-InstalledPackageRoot } catch { $rejected=$true }
if(-not $rejected){throw 'Accepted package for different installed plugin'}
'PASS: package for different installed plugin rejected'
