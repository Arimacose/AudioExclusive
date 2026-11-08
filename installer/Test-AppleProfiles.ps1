[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$TestDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Manage-AudioExclusive.ps1') -LibraryOnly
if(Test-Path -LiteralPath $TestDirectory){throw 'Use a fresh test directory'}
[IO.Directory]::CreateDirectory($TestDirectory)|Out-Null
$script:Meta=Read-Json (Join-Path $PSScriptRoot 'generated/payload/bundle.json')
$checks=New-Object 'Collections.Generic.List[object]'
function Assert-ProfileCase([string]$Name,[bool]$Pass){if(-not $Pass){throw ('FAILED: '+$Name)};$checks.Add([pscustomobject]@{case=$Name;passed=$true})}
# Exercise complete client profiles, including a mixed-dependency update. Mock
# only filesystem hash reads; do not copy or modify the installed Apple files.
$script:OriginalHash=${function:Test-Hash}
$script:Fixture=[IO.Path]::GetFullPath((Join-Path $TestDirectory 'client'))
[IO.Directory]::CreateDirectory($script:Fixture)|Out-Null
$exe=Join-Path $script:Fixture 'AppleMusic.exe'
[IO.File]::WriteAllText($exe,'fixture')
$script:Available=@{}
function Test-Hash([string]$Path,[string]$Hash){
    if([IO.Path]::GetDirectoryName($Path) -eq $script:Fixture){$name=[IO.Path]::GetFileName($Path);return $script:Available.ContainsKey($name) -and $script:Available[$name] -eq $Hash}
    & $script:OriginalHash $Path $Hash
}
$profiles=@($script:Meta.applications.AppleMusic.profiles)
foreach($profile in $profiles){
    $script:Available=@{}
    foreach($property in $profile.fingerprints.PSObject.Properties){$script:Available[$property.Name]=$property.Value}
    Assert-ProfileCase ('accept complete '+$profile.version) (Test-Profile AppleMusic $exe)
    $script:Available['CoreMedia.dll']='00'*32
    Assert-ProfileCase ('refuse altered CoreMedia '+$profile.version) (-not(Test-Profile AppleMusic $exe))
}
$script:Available=@{}
foreach($property in $profiles[1].fingerprints.PSObject.Properties){$script:Available[$property.Name]=$property.Value}
$script:Available['AMPLibraryAgent.exe']=$profiles[0].fingerprints.'AMPLibraryAgent.exe'
Assert-ProfileCase 'refuse mixed old agent and new frameworks' (-not(Test-Profile AppleMusic $exe))
$script:Available['AMPLibraryAgent.exe']=$profiles[1].fingerprints.'AMPLibraryAgent.exe'
$script:Available.Remove('CoreAudioToolbox.dll')
Assert-ProfileCase 'refuse missing new audio toolbox' (-not(Test-Profile AppleMusic $exe))
$report=New-UnsupportedReport AppleMusic @($exe) 'future-build'
Assert-ProfileCase 'diagnostics list both supported versions and detected version' ($report.reason.Contains('1.1540.23042.0') -and $report.reason.Contains('1.1792.39682.0') -and $report.reason.Contains('1.7.0.155') -and $report.reason.Contains('future-build'))
$checks|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $TestDirectory 'results.json') -Encoding utf8
$checks|Format-Table -AutoSize
