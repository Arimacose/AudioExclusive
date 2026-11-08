[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$TestDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Manage-AudioExclusive.ps1') -LibraryOnly
$root=[IO.Path]::GetFullPath($TestDirectory)
if(Test-Path -LiteralPath $root){throw 'Use a fresh workspace test directory'}
[IO.Directory]::CreateDirectory($root)|Out-Null
$script:Meta=Read-Json (Join-Path $PSScriptRoot 'generated/payload/bundle.json')
$checks=New-Object 'Collections.Generic.List[object]'
function Assert-Report([string]$Name,[bool]$Passed){
    if(-not $Passed){throw ('FAILED: '+$Name)}
    $checks.Add([pscustomobject]@{case=$Name;passed=$true})
}
$fixture=Join-Path $root 'unknown-client'
[IO.Directory]::CreateDirectory($fixture)|Out-Null
$windowsShell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
foreach($file in @('cloudmusic.exe','AppleMusic.exe','AMPLibraryAgent.exe')){
    Copy-Item -LiteralPath $windowsShell -Destination (Join-Path $fixture $file)
}
$actualVersion=[Diagnostics.FileVersionInfo]::GetVersionInfo($windowsShell).FileVersion.Trim()
$ncPath=Join-Path $fixture 'cloudmusic.exe'
Assert-Report 'unknown build still rejected by original fingerprint guard' (-not(Test-Profile 'NetEase' $ncPath))
$known=Find-Application 'NetEase'
if($known.status -ne 'Supported'){throw 'This local acceptance case requires a currently supported NetEase executable'}
$sameVersionFolder=Join-Path $root 'same-version-changed-files'
[IO.Directory]::CreateDirectory($sameVersionFolder)|Out-Null
$sameVersionPath=Join-Path $sameVersionFolder 'cloudmusic.exe'
Copy-Item -LiteralPath $known.path -Destination $sameVersionPath
foreach($file in @('cloudmusic.dll','libcef.dll')){[IO.File]::WriteAllText((Join-Path $sameVersionFolder $file),'changed fixture bytes')}
$sameVersionReport=New-UnsupportedReport 'NetEase' @($sameVersionPath)
$knownVersion=[Diagnostics.FileVersionInfo]::GetVersionInfo($known.path).FileVersion.Trim()
Assert-Report 'supported version label with changed dependencies is refused and reports detected label' (-not(Test-Profile 'NetEase' $sameVersionPath) -and $sameVersionReport.reason.Contains($knownVersion) -and $sameVersionReport.reason.Contains('SHA256'))
$nc=New-UnsupportedReport 'NetEase' @($ncPath)
Assert-Report 'NetEase reason names unsupported current build and both supported versions' ($nc.reason.Contains('当前版本不支持') -and $nc.reason.Contains('3.1.36.205322') -and $nc.reason.Contains('3.1.41.205564'))
Assert-Report 'NetEase reports actual PE file version and path' ($nc.reason.Contains($actualVersion) -and $nc.detectedVersions[0].path -eq $ncPath)
Assert-Report 'same version still requires fingerprints is explicit' ($nc.reason.Contains('版本号相同也必须通过文件指纹校验'))
$missing=New-UnsupportedReport 'NetEase' @((Join-Path $fixture 'missing/cloudmusic.exe'))
Assert-Report 'unreadable version is explicit instead of fabricated' ($missing.reason.Contains('文件不存在'))
$multiple=New-UnsupportedReport 'NetEase' @($ncPath,(Join-Path $fixture 'missing/cloudmusic.exe'))
Assert-Report 'multiple detected installations retained' ($multiple.detectedVersions.Count -eq 2 -and $multiple.reason.Contains('另一个安装'))
$applePath=Join-Path $fixture 'AppleMusic.exe'
$apple=New-UnsupportedReport 'AppleMusic' @($applePath) '9.9.9.9'
Assert-Report 'Apple reports supported package and file versions' ($apple.reason.Contains('1.1540.23042.0') -and $apple.reason.Contains('1.6.4.90'))
Assert-Report 'Apple reports actual package, main and agent versions' ($apple.reason.Contains('软件包版本 9.9.9.9') -and $apple.reason.Contains('AppleMusic.exe 文件版本 '+$actualVersion) -and $apple.reason.Contains('AMPLibraryAgent.exe 文件版本 '+$actualVersion))
# Execute real Apple discovery against a simulated Appx package; no app writes.
function Get-AppxPackage {[pscustomobject]@{InstallLocation=$fixture;Version='9.9.9.9';PackageFamilyName='Fixture.AppleMusic'}}
$found=Find-Application 'AppleMusic'
Assert-Report 'real discovery attaches diagnostics on rejection' ($found.status -eq 'Unsupported' -and $found.detectedVersions.Count -eq 1 -and $found.reason.Contains('9.9.9.9'))
$script:fixtureReports=@{NetEase=$nc;AppleMusic=$apple}
function Find-Application([string]$Name){
    if($Name -eq 'AppleMusic'){return [pscustomobject]@{name=$Name;status='Supported';path=$applePath;aumid='';reason=''}}
    $report=$script:fixtureReports[$Name]
    return [pscustomobject]@{name=$Name;status='Unsupported';path=$ncPath;aumid='';reason=$report.reason;supportedVersions=$report.supportedVersions;detectedVersions=$report.detectedVersions}
}
function Install-Application([string]$Name,$Found){[pscustomobject]@{app=$Name;result='Installed'}}
$results=@(Invoke-ApplicationActions 'Install' 'All')
Assert-Report 'unsupported app continues to second app and structured result keeps versions' ($results.Count -eq 2 -and $results[0].result -eq 'Unsupported' -and $results[0].supportedVersions.Count -eq 2 -and $results[0].detectedVersions[0].version.Contains($actualVersion) -and $results[1].result -eq 'Installed')
$results|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $root 'skip-result.json') -Encoding utf8
# Sandbox the automatic-start script only by rebasing its own-directory guard.
# The real mismatch branch runs; any attempt to start a process fails the test.
$helper=Join-Path $root 'Start-VerifiedAppleCompanion.ps1'
$helperText=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'Start-VerifiedAppleCompanion.ps1'))
$guard="`$expected=[IO.Path]::GetFullPath((Join-Path `$env:PUBLIC 'AMExclusive'))"
if(-not $helperText.Contains($guard)){throw 'Automatic-start directory guard changed; inspect test fixture'}
$helperText=$helperText.Replace($guard,("`$expected='"+$root.Replace("'","''")+"'"))
[IO.File]::WriteAllText($helper,$helperText,(New-Object Text.UTF8Encoding($true)))
function Start-Process {throw 'TEST FAILURE: refused build tried to start a process'}
& $helper
$helperLog=[IO.File]::ReadAllText((Join-Path $root 'verified-startup.log'))
Assert-Report 'automatic Apple rejection logs supported and detected versions before returning' ($helperLog.Contains('当前版本不支持') -and $helperLog.Contains('1.1540.23042.0') -and $helperLog.Contains('1.6.4.90') -and $helperLog.Contains('9.9.9.9') -and $helperLog.Contains($actualVersion) -and $helperLog.Contains('已跳过注入'))
Assert-Report 'automatic Apple rejection names failed client file' ($helperLog -match '(AppleMusic.exe|AMPLibraryAgent.exe|CoreMedia.dll) 文件缺失或 SHA256')
$checks|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $root 'results.json') -Encoding utf8
$checks|Format-Table -AutoSize
