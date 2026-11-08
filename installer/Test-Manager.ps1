[CmdletBinding()]
param([string]$TestDirectory)
$ErrorActionPreference='Stop'
if(-not $TestDirectory){throw 'Pass a dedicated workspace test directory'}
. (Join-Path $PSScriptRoot 'Manage-AudioExclusive.ps1') -LibraryOnly
$testRoot=[IO.Path]::GetFullPath($TestDirectory)
if(Test-Path -LiteralPath $testRoot){throw 'Use a fresh directory; tests do not delete existing files'}
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
$script:ManagerRoot=$testRoot
$script:BackupRoot=Join-Path $testRoot 'backups'
$script:StateRoot=Join-Path $testRoot 'state'
$script:NetEaseInstall=Join-Path $testRoot 'apps\NetEase'
$script:AppleInstall=Join-Path $testRoot 'apps\AppleMusic'
$script:ShortcutRoot=Join-Path $testRoot 'shortcuts'
$testResults=New-Object 'Collections.Generic.List[object]'
function Assert-Case([string]$Name,[bool]$Passed){
    if(-not $Passed){throw ('FAILED: '+$Name)}
    $testResults.Add([pscustomobject]@{case=$Name;passed=$true})
}
# Exercise the real orchestrator while replacing app discovery and mutation.
# These controlled branches never alter native application installations.
$script:Scenario='missing';$script:Visits=@()
function Find-Application([string]$Name){
    $status=if($Name -eq 'NetEase' -and $script:Scenario -eq 'missing'){'NotFound'}elseif($Name -eq 'NetEase' -and $script:Scenario -eq 'unsupported'){'Unsupported'}else{'Supported'}
    [pscustomobject]@{name=$Name;status=$status;path='C:\fixture\client.exe';aumid='';reason='controlled fixture'}
}
$realInstall=${function:Install-Application}
function Install-Application([string]$Name,$Found){
    $script:Visits+=$Name
    if($Name -eq 'NetEase' -and $script:Scenario -eq 'failure'){throw 'controlled app failure'}
    [pscustomobject]@{app=$Name;result='Installed'}
}
$branch=@(Invoke-ApplicationActions 'Install' 'All')
Assert-Case 'missing first app still installs second' ($branch.Count -eq 2 -and $branch[0].result -eq 'NotFound' -and $branch[1].result -eq 'Installed' -and $script:Visits.Count -eq 1)
$script:Scenario='unsupported';$script:Visits=@();$branch=@(Invoke-ApplicationActions 'Install' 'All')
Assert-Case 'unsupported first app still installs second' ($branch.Count -eq 2 -and $branch[0].result -eq 'Unsupported' -and $branch[1].result -eq 'Installed')
$script:Scenario='failure';$script:Visits=@();$branch=@(Invoke-ApplicationActions 'Install' 'All')
Assert-Case 'failed first app still installs second' ($branch.Count -eq 2 -and $branch[0].result -eq 'Failed' -and $branch[1].result -eq 'Installed' -and $script:Visits.Count -eq 2)
$realFind=${function:Find-Application}
function Find-Application([string]$Name){[pscustomobject]@{name=$Name;status='NotFound';path='';aumid='';reason='controlled fixture'}}
$branch=@(Invoke-ApplicationActions 'Install' 'All')
Assert-Case 'both apps absent completes with two skips' ($branch.Count -eq 2 -and @($branch|Where-Object result -NE 'NotFound').Count -eq 0)
${function:Install-Application}=$realInstall
Assert-Case 'ordinary child path accepted' ((Assert-Under (Join-Path $testRoot 'safe\file.bin') $testRoot).StartsWith($testRoot))
$rejected=$false;try{Assert-Under (Join-Path $testRoot '..\outside.bin') $testRoot|Out-Null}catch{$rejected=$true}
Assert-Case 'parent traversal rejected before writes' $rejected
$aclFixture=Join-Path $testRoot 'new-apple-acl'
[IO.Directory]::CreateDirectory($aclFixture)|Out-Null
Set-NewAppleFolderAcl $aclFixture
$fixtureAcl=Get-Acl -LiteralPath $aclFixture
$packageRules=@($fixtureAcl.GetAccessRules($true,$false,[Security.Principal.SecurityIdentifier])|Where-Object {$_.IdentityReference.Value -eq 'S-1-15-2-1'})
Assert-Case 'fresh Apple folder grants package read without write' ($fixtureAcl.AreAccessRulesProtected -and $packageRules.Count -eq 1 -and -not($packageRules[0].FileSystemRights -band [Security.AccessControl.FileSystemRights]::WriteData))
$script:BundlePath=Join-Path $testRoot 'bad.zip';$script:BundleHash=('0'*64)
[IO.File]::WriteAllText($script:BundlePath,'not an authenticated payload')
$rejected=$false;try{Initialize-Payload}catch{$rejected=$true}
Assert-Case 'tampered bundle rejected before app deployment' ($rejected -and -not(Test-Path -LiteralPath $script:NetEaseInstall))
# A genuine failed transaction on fixture files: the wrapper must restore
# configuration, plugin bytes, state and ACL, preserving its failed staging.
function Stop-AppRuntime($Found){$script:Stopped+=$Found.name}
$script:Stopped=@()
[IO.Directory]::CreateDirectory($script:NetEaseInstall)|Out-Null
[IO.File]::WriteAllText((Join-Path $script:NetEaseInstall 'fixture.dll'),'original plugin')
[IO.File]::WriteAllText((Join-Path $script:NetEaseInstall 'settings.ini'),"[exclusive]`r`nenabled=0`r`nuser_choice=keep`r`n")
$iniBefore=(Get-FileHash -LiteralPath (Join-Path $script:NetEaseInstall 'settings.ini')).Hash
$state=[pscustomobject]@{name='NetEase';enabled=$true;target='C:\fixture\client.exe';aumid='';backup='';disabledFolder='';shortcut=$null;package='';updated='fixture'}
Save-State 'NetEase' $state
Save-State 'AppleMusic' ([pscustomobject]@{name='AppleMusic';enabled=$true;target='C:\fixture\apple.exe';aumid='';backup='';disabledFolder='';shortcut=$null;package='';updated='fixture'})
$appleStateBefore=(Get-FileHash -LiteralPath (Join-Path $script:StateRoot 'AppleMusic.json')).Hash
function Install-ApplicationCore([string]$Name,$Found){
    [IO.File]::WriteAllText((Join-Path $script:NetEaseInstall 'fixture.dll'),'incomplete update')
    [IO.File]::WriteAllText((Join-Path $script:NetEaseInstall 'settings.ini'),'incomplete update')
    throw 'controlled write failure'
}
$rejected=$false
try{Install-Application 'NetEase' ([pscustomobject]@{name='NetEase';path='C:\fixture\client.exe';aumid=''})|Out-Null}catch{$rejected=$_.Exception.Message.StartsWith('安装失败，已恢复操作前状态：')}
Assert-Case 'failed deployment reports successful rollback' $rejected
Assert-Case 'rollback restores plugin bytes and user config' ([IO.File]::ReadAllText((Join-Path $script:NetEaseInstall 'fixture.dll')) -eq 'original plugin' -and (Get-FileHash -LiteralPath (Join-Path $script:NetEaseInstall 'settings.ini')).Hash -eq $iniBefore -and (Get-State 'NetEase').enabled)
$removed=Remove-Application 'NetEase'
Assert-Case 'individual cancellation preserves other app state' ($removed.result -eq 'Removed' -and (Get-FileHash -LiteralPath (Join-Path $script:StateRoot 'AppleMusic.json')).Hash -eq $appleStateBefore)
Assert-Case 'cancellation archives files without deleting config' (-not(Test-Path -LiteralPath $script:NetEaseInstall) -and (Get-FileHash -LiteralPath (Join-Path $removed.backup 'settings.ini')).Hash -eq $iniBefore -and -not (Get-State 'NetEase').enabled)
$twice=Remove-Application 'NetEase'
Assert-Case 'repeat cancellation is idempotent' ($twice.result -eq 'AlreadyRemoved')
[IO.Directory]::CreateDirectory($script:AppleInstall)|Out-Null
[IO.File]::WriteAllText((Join-Path $script:AppleInstall 'settings.ini'),'preserve apple fixture')
function Get-ScheduledTask {return [pscustomobject]@{Actions=[pscustomobject]@{Execute='C:\unrelated.exe';Arguments=''}}}
$rejected=$false;try{Remove-Application 'AppleMusic'|Out-Null}catch{$rejected=$_.Exception.Message.Contains('同名计划任务指向其他程序')}
Assert-Case 'unrelated scheduled task blocks cancellation before file changes' ($rejected -and (Test-Path -LiteralPath $script:AppleInstall) -and (Get-State 'AppleMusic').enabled)
function Get-ScheduledTask {return [pscustomobject]@{Actions=[pscustomobject]@{Execute=(Get-ShellPath);Arguments=(Get-AppleHelperArguments)}}}
function Disable-ScheduledTask {throw [UnauthorizedAccessException]::new('controlled task access denied')}
function Stop-ScheduledTask {throw [UnauthorizedAccessException]::new('controlled task access denied')}
$netEaseBeforeAppleRemove=(Get-FileHash -LiteralPath (Join-Path $script:StateRoot 'NetEase.json')).Hash
$appleRemoved=Remove-Application 'AppleMusic'
Assert-Case 'task access denial still disables Apple plugin files with explicit reason' ($appleRemoved.result -eq 'Removed' -and $appleRemoved.companionTaskNote.Contains('controlled task access denied') -and -not(Test-Path -LiteralPath $script:AppleInstall) -and -not(Get-State 'AppleMusic').enabled -and [IO.File]::ReadAllText((Join-Path $appleRemoved.backup 'settings.ini')) -eq 'preserve apple fixture')
Assert-Case 'Apple cancellation preserves NetEase state' ((Get-FileHash -LiteralPath (Join-Path $script:StateRoot 'NetEase.json')).Hash -eq $netEaseBeforeAppleRemove)
$oldLocalAppData=$env:LOCALAPPDATA
try{
    $env:LOCALAPPDATA=$testRoot
    $bridgeDirectory=Join-Path $testRoot 'NCExclusive'
    [IO.Directory]::CreateDirectory($bridgeDirectory)|Out-Null
    $bridgeLog=Join-Path $bridgeDirectory ('plugin-'+$PID+'.jsonl')
    Assert-Case 'missing frontend log is not ready' ((Get-NetEaseBridgeState $PID) -eq 'Waiting')
    [IO.File]::WriteAllText($bridgeLog,'{"event":"cef_hooks_ready"}')
    Assert-Case 'loaded CEF hooks without JS acknowledgement are not ready' ((Get-NetEaseBridgeState $PID) -eq 'Waiting')
    [IO.File]::WriteAllText($bridgeLog,'{"event":"settings_script_running"}')
    Assert-Case 'JS acknowledgement makes frontend ready' ((Get-NetEaseBridgeState $PID) -eq 'Ready')
    Assert-Case 'stale PID log from before process start is ignored' ((Get-NetEaseBridgeState $PID ((Get-Date).AddMinutes(1))) -eq 'Waiting')
    [IO.File]::WriteAllText($bridgeLog,'{"event":"settings_bootstrap_timeout"}')
    Assert-Case 'native bootstrap timeout reports frontend failure' ((Get-NetEaseBridgeState $PID) -eq 'Failed')
    $rejected=$false;try{Wait-NetEaseBridge $PID ([datetime]::MinValue) 0}catch{$rejected=$_.Exception.Message.Contains('设置页脚本未确认启动')}
    Assert-Case 'frontend failure gives an explicit reason instead of DLL success' $rejected
}finally{$env:LOCALAPPDATA=$oldLocalAppData}
$testResults|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $testRoot 'results.json') -Encoding utf8
$testResults|Format-Table -AutoSize
