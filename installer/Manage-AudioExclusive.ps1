[CmdletBinding()]
param(
    [ValidateSet('Auto','Install','Status','Remove','Launch','Menu')][string]$Action='Auto',
    [ValidateSet('All','NetEase','AppleMusic')][string]$App='All',
    [string]$BundlePath='', [string]$BundleHash='', [switch]$NoMenu,
    [switch]$LibraryOnly
)
$ErrorActionPreference='Stop'
# A native bootstrap can inherit PSModulePath from PowerShell 7 or another
# host. Put this process's own built-in modules first, especially in 5.1.
$moduleDirectories=@((Join-Path $PSHOME 'Modules'),(Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules'))
$moduleDirectories+=@($env:PSModulePath -split ';'|Where-Object {$_})
$env:PSModulePath=($moduleDirectories|Select-Object -Unique)-join ';'
Set-StrictMode -Version 2
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
$OutputEncoding=[Console]::OutputEncoding
$script:ManagerRoot=Join-Path $env:LOCALAPPDATA 'AudioExclusive'
$script:BackupRoot=Join-Path $script:ManagerRoot 'backups'
$script:StateRoot=Join-Path $script:ManagerRoot 'state'
$script:SourceRoot=''
$script:Meta=$null
$script:Utf8=New-Object Text.UTF8Encoding($false)
$script:TaskName='AMExclusive-Companion-'+[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$script:AppleInstall=Join-Path $env:PUBLIC 'AMExclusive'
$script:NetEaseInstall=Join-Path $script:ManagerRoot 'apps\NetEase'
$script:ShortcutRoot=Join-Path ([Environment]::GetFolderPath('Programs')) 'AudioExclusive'
$script:Labels=@{NetEase='网易云音乐';AppleMusic='Apple Music'}

function Write-Step([string]$Message){Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'),$Message)}
function Read-Json([string]$Path){Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json}
function Write-Json([string]$Path,$Value){
    $parent=Split-Path -Parent $Path
    if(-not(Test-Path -LiteralPath $parent)){[IO.Directory]::CreateDirectory($parent)|Out-Null}
    $temp=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    [IO.File]::WriteAllText($temp,($Value|ConvertTo-Json -Depth 20),$script:Utf8)
    Move-Item -LiteralPath $temp -Destination $Path -Force
}
function Test-Hash([string]$Path,[string]$Hash){
    (Test-Path -LiteralPath $Path -PathType Leaf) -and ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Hash)
}
function Assert-PlainDirectory([string]$Path){
    $next=[IO.Path]::GetFullPath($Path)
    while($next){
        if(Test-Path -LiteralPath $next){
            if((Get-Item -LiteralPath $next -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "拒绝操作重解析目录：$next"}
        }
        $parent=Split-Path -Parent $next
        if($parent -eq $next){break};$next=$parent
    }
}
function Assert-Under([string]$Path,[string]$Root){
    $absolute=[IO.Path]::GetFullPath($Path)
    $base=[IO.Path]::GetFullPath($Root).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)){throw "路径超出管理范围：$absolute"}
    Assert-PlainDirectory $absolute
    return $absolute
}
function Get-State([string]$Name){
    $path=Join-Path $script:StateRoot ($Name+'.json')
    if(Test-Path -LiteralPath $path){return Read-Json $path}
    return $null
}
function Save-State([string]$Name,$State){Write-Json (Join-Path $script:StateRoot ($Name+'.json')) $State}
function Get-ShellPath {
    $candidate=Join-Path $env:ProgramFiles 'PowerShell\7\pwsh.exe'
    if(Test-Path -LiteralPath $candidate){return $candidate}
    return (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe')
}
function Resolve-InstalledPackageRoot {
    $recordPath=Join-Path $script:ManagerRoot 'package.json'
    $record=$null
    if(Test-Path -LiteralPath $recordPath){$record=Read-Json $recordPath}
    if($record -and $record.PSObject.Properties['path']){
        $current=Assert-Under $record.path (Join-Path $script:ManagerRoot 'packages')
        if(Test-Path -LiteralPath (Join-Path $current 'bundle.json')){return $current}
        Write-Step '原插件包目录已失效，正在恢复已安装版本的启动入口。'
    }
    $candidates=New-Object 'Collections.Generic.List[string]'
    foreach($name in @('NetEase','AppleMusic')){
        $state=Get-State $name
        if($state -and $state.PSObject.Properties['package'] -and $state.package){$candidates.Add([string]$state.package)}
    }
    $packageRoot=Join-Path $script:ManagerRoot 'packages'
    if(Test-Path -LiteralPath $packageRoot){
        foreach($directory in @(Get-ChildItem -LiteralPath $packageRoot -Directory | Sort-Object LastWriteTime -Descending)){
            if($directory.Name -notlike 'stage-*'){$candidates.Add($directory.FullName)}
        }
    }
    foreach($candidate in @($candidates | Select-Object -Unique)){
        try {
            $candidate=Assert-Under $candidate $packageRoot
            $manifest=Join-Path $candidate 'bundle.json'
            if(-not(Test-Path -LiteralPath $manifest)){continue}
            $meta=Read-Json $manifest
            if(-not $meta.files -or -not $meta.applications){continue}
            $valid=$true
            foreach($file in $meta.files){
                $payload=Assert-Under (Join-Path $candidate $file.path) $candidate
                if(-not(Test-Hash $payload $file.sha256)){$valid=$false;break}
            }
            if(-not $valid){continue}
            # Recovery must select the package matching the installed plugin,
            # rather than silently switching an existing install to older code.
            $matched=0
            foreach($name in @('NetEase','AppleMusic')){
                if($App -ne 'All' -and $App -ne $name){continue}
                $state=Get-State $name
                if(-not $state -or -not $state.enabled){continue}
                foreach($file in $meta.applications.$name.files){
                    if(-not(Test-Hash (Join-Path (Get-InstallPath $name) $file.name) $file.sha256)){$valid=$false;break}
                }
                if(-not $valid){break}
                $matched++
            }
            if(-not $valid -or $matched -eq 0){continue}
            if(Test-Path -LiteralPath $recordPath){
                $backup=Join-Path $script:BackupRoot ('package-reference-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)+'.json')
                [IO.Directory]::CreateDirectory($script:BackupRoot)|Out-Null
                Copy-Item -LiteralPath $recordPath -Destination $backup
            }
            Write-Json $recordPath @{path=$candidate;sha256=(Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.ToLowerInvariant()}
            Write-Step ('已恢复插件包入口：'+$meta.build)
            return $candidate
        } catch { Write-Verbose ('跳过插件包候选：'+$_.Exception.Message) }
    }
    throw '启动所需的插件包已被移除，现存包与已安装插件不匹配。请在独占插件管理中重新安装。'
}

function Initialize-Payload {
    Assert-PlainDirectory $script:ManagerRoot
    [IO.Directory]::CreateDirectory($script:ManagerRoot)|Out-Null
    if($BundlePath){
        if($BundleHash -notmatch '^[A-Fa-f0-9]{64}$' -or -not(Test-Hash $BundlePath $BundleHash)){throw '内嵌插件包 SHA256 校验失败，未进行应用改动。'}
        $script:SourceRoot=Join-Path $script:ManagerRoot ('packages\'+$BundleHash.ToLowerInvariant())
        Assert-Under $script:SourceRoot $script:ManagerRoot|Out-Null
        if(-not(Test-Path -LiteralPath (Join-Path $script:SourceRoot 'bundle.json'))){
            $stage=Join-Path $script:ManagerRoot ('packages\stage-'+[Guid]::NewGuid().ToString('N'))
            [IO.Directory]::CreateDirectory($stage)|Out-Null
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip=[IO.Compression.ZipFile]::OpenRead($BundlePath)
            try{
                foreach($entry in $zip.Entries){
                    if(-not $entry.Name){continue}
                    $destination=Assert-Under (Join-Path $stage $entry.FullName) $stage
                    [IO.Directory]::CreateDirectory((Split-Path -Parent $destination))|Out-Null
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$destination,$false)
                }
            }finally{$zip.Dispose()}
            $staged=Read-Json (Join-Path $stage 'bundle.json')
            foreach($file in $staged.files){
                $candidate=Assert-Under (Join-Path $stage $file.path) $stage
                if(-not(Test-Hash $candidate $file.sha256)){throw "插件文件校验失败：$($file.path)"}
            }
            Move-Item -LiteralPath $stage -Destination $script:SourceRoot
        }
        Write-Json (Join-Path $script:ManagerRoot 'package.json') @{path=$script:SourceRoot;sha256=$BundleHash}
        $persistedScript=Join-Path $script:ManagerRoot 'Manage-AudioExclusive.ps1'
        if([IO.Path]::GetFullPath($PSCommandPath) -ne [IO.Path]::GetFullPath($persistedScript)){
            Copy-Item -LiteralPath $PSCommandPath -Destination $persistedScript -Force
        }
    }else{
        $script:SourceRoot=Resolve-InstalledPackageRoot
    }
    $script:Meta=Read-Json (Join-Path $script:SourceRoot 'bundle.json')
    foreach($file in $script:Meta.files){
        $candidate=Assert-Under (Join-Path $script:SourceRoot $file.path) $script:SourceRoot
        if(-not(Test-Hash $candidate $file.sha256)){throw "已解包文件校验失败：$($file.path)"}
    }
    Write-Step '插件包校验通过；只包含网易云和 Apple Music，不含 Spotify。'
}
function Test-Profile([string]$Name,[string]$Executable){
    if(-not(Test-Path -LiteralPath $Executable -PathType Leaf)){return $false}
    $directory=Split-Path -Parent $Executable
    $profiles=@($script:Meta.applications.$Name.fingerprints)
    if($script:Meta.applications.$Name.PSObject.Properties['profiles']){
        $profiles=@($script:Meta.applications.$Name.profiles|ForEach-Object {$_.fingerprints})
    }
    foreach($profile in $profiles){
        $matched=$true
        foreach($property in $profile.PSObject.Properties){
            if(-not(Test-Hash (Join-Path $directory $property.Name) $property.Value)){$matched=$false;break}
        }
        if($matched){return $true}
    }
    return $false
}
function Get-ClientVersionInfo([string]$Name,[string]$Executable,[string]$PackageVersion=''){
    $versions=New-Object 'Collections.Generic.List[string]'
    if($Name -eq 'AppleMusic'){
        if(-not $PackageVersion){$PackageVersion='未能读取'}
        $versions.Add('软件包版本 '+$PackageVersion)
    }
    $directory=Split-Path -Parent $Executable
    $files=if($Name -eq 'AppleMusic'){@('AppleMusic.exe','AMPLibraryAgent.exe')}else{@('cloudmusic.exe')}
    foreach($file in $files){
        $path=Join-Path $directory $file
        $version='未能读取'
        try{
            if(Test-Path -LiteralPath $path -PathType Leaf){
                $value=[Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileVersion
                if($value){$version=$value.Trim()}
            }else{$version='文件不存在'}
        }catch{$version='读取失败：'+$_.Exception.Message}
        $versions.Add($file+' 文件版本 '+$version)
    }
    return ($versions -join '；')
}
function Get-SupportedVersions([string]$Name){
    $meta=$script:Meta.applications.$Name
    if($meta.PSObject.Properties['supportedVersions']){return @($meta.supportedVersions)}
    return @($meta.version)
}
function New-UnsupportedReport([string]$Name,[string[]]$Executables,[string]$PackageVersion=''){
    $supported=@(Get-SupportedVersions $Name)
    $detected=@(foreach($executable in $Executables){
        [pscustomobject]@{path=$executable;version=(Get-ClientVersionInfo $Name $executable $PackageVersion)}
    })
    $versionText=($detected|ForEach-Object {$_.version+'（路径：'+$_.path+'）'}) -join '；另一个安装：'
    return [pscustomobject]@{
        supportedVersions=$supported
        detectedVersions=$detected
        reason=('当前版本不支持。已支持版本：'+($supported -join '、')+'。当前探测到的版本：'+$versionText+'。原因：关键客户端文件缺失或 SHA256 与所有已验证构建不匹配；版本号相同也必须通过文件指纹校验。')
    }
}
function Find-Application([string]$Name){
    if($Name -eq 'AppleMusic'){
        $package=Get-AppxPackage -Name AppleInc.AppleMusicWin -ErrorAction SilentlyContinue | Sort-Object Version -Descending | Select-Object -First 1
        if(-not $package){return [pscustomobject]@{status='NotFound';name=$Name;path='';aumid='';reason='未找到当前用户安装的 Apple Music'}}
        $path=Join-Path $package.InstallLocation 'AppleMusic.exe'
        $supported=Test-Profile $Name $path
        if($supported){return [pscustomobject]@{status='Supported';name=$Name;path=$path;aumid=($package.PackageFamilyName+'!App');reason=''}}
        $report=New-UnsupportedReport $Name @($path) ([string]$package.Version)
        return [pscustomobject]@{status='Unsupported';name=$Name;path=$path;aumid=($package.PackageFamilyName+'!App');reason=$report.reason;supportedVersions=$report.supportedVersions;detectedVersions=$report.detectedVersions}
    }
    $candidates=New-Object 'Collections.Generic.List[string]'
    foreach($process in @(Get-CimInstance Win32_Process -Filter "Name='cloudmusic.exe'" -ErrorAction SilentlyContinue)){
        if($process.ExecutablePath){$candidates.Add($process.ExecutablePath)}
    }
    foreach($key in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\cloudmusic.exe','HKLM:\Software\Microsoft\Windows\CurrentVersion\App Paths\cloudmusic.exe','HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\cloudmusic.exe')){
        if(Test-Path -LiteralPath $key){$value=(Get-Item -LiteralPath $key).GetValue('');if($value){$candidates.Add($value)}}
    }
    foreach($key in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*')){
        foreach($record in @(Get-ItemProperty -Path $key -ErrorAction SilentlyContinue)){
            if($record.PSObject.Properties['DisplayName'] -and $record.DisplayName -match '网易云|NetEase.*Music|CloudMusic'){
                if($record.PSObject.Properties['InstallLocation'] -and $record.InstallLocation){$candidates.Add((Join-Path $record.InstallLocation 'cloudmusic.exe'))}
                if($record.PSObject.Properties['DisplayIcon'] -and $record.DisplayIcon){
                    $icon=[regex]::Match($record.DisplayIcon,'^\s*"?(.+?\.exe)"?(?:,.*)?$')
                    if($icon.Success){$candidates.Add($icon.Groups[1].Value)}
                }
            }
        }
    }
    foreach($directory in @($env:ProgramFiles,${env:ProgramFiles(x86)},$env:LOCALAPPDATA,(Join-Path $env:LOCALAPPDATA 'Programs'))){
        if($directory){$candidates.Add((Join-Path $directory 'NetEase\CloudMusic\cloudmusic.exe'))}
    }
    $existing=@($candidates|Select-Object -Unique|Where-Object {Test-Path -LiteralPath $_ -PathType Leaf})
    foreach($candidate in $existing){
        if(Test-Profile $Name $candidate){return [pscustomobject]@{status='Supported';name=$Name;path=[IO.Path]::GetFullPath($candidate);aumid='';reason=''}}
    }
    if(-not $existing.Count){return [pscustomobject]@{status='NotFound';name=$Name;path='';aumid='';reason='未找到网易云音乐'}}
    $report=New-UnsupportedReport $Name $existing
    return [pscustomobject]@{status='Unsupported';name=$Name;path=$existing[0];aumid='';reason=$report.reason;supportedVersions=$report.supportedVersions;detectedVersions=$report.detectedVersions}
}
function Get-InstallPath([string]$Name){if($Name -eq 'NetEase'){return $script:NetEaseInstall};return $script:AppleInstall}
function Get-AppleHelperArguments {
    return ('-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+(Join-Path $script:AppleInstall 'Start-VerifiedAppleCompanion.ps1')+'"')
}
function Test-OwnedAppleTask($Task){
    if(@($Task.Actions).Count -ne 1){return $false}
    if($Task.Actions.Execute -eq (Join-Path $script:AppleInstall 'am-exclusive-broker.exe')){return $true}
    return ($Task.Actions.Execute -eq (Get-ShellPath) -and $Task.Actions.Arguments -eq (Get-AppleHelperArguments))
}
function Get-AppProcesses($Found){
    $paths=@($Found.path)
    if($Found.name -eq 'AppleMusic'){$paths+=Join-Path (Split-Path -Parent $Found.path) 'AMPLibraryAgent.exe';$paths+=Join-Path $script:AppleInstall 'am-exclusive-broker.exe'}
    return @(Get-CimInstance Win32_Process -Filter "Name='cloudmusic.exe' OR Name='AppleMusic.exe' OR Name='AMPLibraryAgent.exe' OR Name='am-exclusive-broker.exe'" -ErrorAction SilentlyContinue | Where-Object {$_.ExecutablePath -and $_.ExecutablePath -in $paths})
}
function Stop-AppRuntime($Found){
    $processes=@(Get-AppProcesses $Found)
    if(-not $processes.Count){return}
    Write-Step ('停止 {0} 的插件运行进程，释放已载入的 DLL。' -f $script:Labels[$Found.name])
    foreach($process in $processes){
        $running=Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
        if($running){$null=$running.CloseMainWindow()}
    }
    Start-Sleep -Milliseconds 700
    foreach($process in @(Get-AppProcesses $Found)){Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop}
    $deadline=(Get-Date).AddSeconds(6)
    while(@(Get-AppProcesses $Found).Count){if((Get-Date) -gt $deadline){throw '应用进程未能退出'};Start-Sleep -Milliseconds 100}
}
function New-AppBackup([string]$Name,[string]$Install){
    $path=Join-Path $script:BackupRoot ($Name+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
    Assert-Under $path $script:BackupRoot|Out-Null
    [IO.Directory]::CreateDirectory($path)|Out-Null
    if(Test-Path -LiteralPath $Install){
        Assert-PlainDirectory $Install
        $links=@(Get-ChildItem -LiteralPath $Install -Force -Recurse|Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint})
        if($links.Count){throw '插件目录包含重解析项，保留原目录并跳过改动'}
        Copy-Item -LiteralPath $Install -Destination (Join-Path $path 'previous-install') -Recurse
        [IO.File]::WriteAllText((Join-Path $path 'previous-install-acl.txt'),(Get-Acl -LiteralPath $Install).Sddl,$script:Utf8)
    }
    if($Name -eq 'AppleMusic'){
        $task=Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
        if($task){
            if(-not(Test-OwnedAppleTask $task)){throw '同名计划任务指向其他程序，未覆盖该任务'}
            [IO.File]::WriteAllText((Join-Path $path 'previous-task.xml'),(Export-ScheduledTask -TaskName $script:TaskName),$script:Utf8)
        }
    }
    return $path
}
function Test-CurrentAppleCompanion {
    # An elevated installation may leave an otherwise correct task that the
    # interactive user can read and run but cannot rewrite. Reuse only the
    # complete expected definition; do not broaden task permissions.
    try {
        $service=New-Object -ComObject Schedule.Service
        $service.Connect()
        $registered=$service.GetFolder('\').GetTask($script:TaskName)
        $definition=$registered.Definition
        if(-not $registered.Enabled -or $definition.Actions.Count -ne 1 -or $definition.Triggers.Count -ne 1){return $false}
        $principal=$definition.Principal
        $userSid=$principal.UserId
        if($userSid -notlike 'S-1-*'){
            $account=New-Object Security.Principal.NTAccount($userSid)
            $userSid=$account.Translate([Security.Principal.SecurityIdentifier]).Value
        }
        if($userSid -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -or $principal.LogonType -ne 3 -or $principal.RunLevel -ne 0){return $false}
        $action=$definition.Actions.Item(1)
        if($action.Type -ne 0 -or $action.Path -ne (Get-ShellPath) -or $action.Arguments -cne (Get-AppleHelperArguments) -or $action.WorkingDirectory -ne $script:AppleInstall){return $false}
        $trigger=$definition.Triggers.Item(1)
        $subscription=@'
<QueryList><Query Id="0" Path="Microsoft-Windows-AppModel-Runtime/Admin"><Select Path="Microsoft-Windows-AppModel-Runtime/Admin">*[System[Provider[@Name='Microsoft-Windows-AppModel-Runtime'] and EventID=201] and EventData[Data[@Name='ImageName']='AppleMusic.exe' and Data[@Name='ApplicationName']='AppleInc.AppleMusicWin_nzyj5cx40ttqa!App']]</Select></Query></QueryList>
'@
        if($trigger.Type -ne 0 -or -not $trigger.Enabled -or $trigger.Delay -ne 'PT15S' -or $trigger.Subscription.Trim() -cne $subscription.Trim()){return $false}
        $settings=$definition.Settings
        return ($settings.MultipleInstances -eq 3 -and $settings.Hidden -and $settings.AllowHardTerminate -and
            $settings.AllowDemandStart -and -not $settings.DisallowStartIfOnBatteries -and
            -not $settings.StopIfGoingOnBatteries -and -not $settings.StartWhenAvailable -and
            -not $settings.RunOnlyIfIdle -and -not $settings.RunOnlyIfNetworkAvailable -and
            $settings.ExecutionTimeLimit -eq 'PT0S')
    } catch { return $false }
}
function Install-Companion {
    if(Test-CurrentAppleCompanion){
        Write-Step ('Apple Music 现有伴随任务与预期定义一致，复用任务：'+$script:TaskName)
        return
    }
    # Keep the verified original task registration logic; it uses only an
    # Apple Music launch event and binds the companion to one host lifetime.
    $register=Join-Path $script:AppleInstall 'Register-Companion.ps1'
    $registration=& $register -InstallDirectory $script:AppleInstall
    $task=Get-ScheduledTask -TaskName $script:TaskName -ErrorAction Stop
    if($task.Settings.Enabled -eq $false -or -not(Test-OwnedAppleTask $task)){throw 'Apple Music 伴随任务核验失败'}
    Write-Step ('Apple Music 伴随任务已启用，冷启动延迟 15 秒：'+$registration.TaskName)
}
function Set-NewAppleFolderAcl([string]$Directory){
    Assert-PlainDirectory $Directory
    $acl=New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    $owner=[Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.SetOwner($owner)
    $inherit=[Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit'
    $propagation=[Security.AccessControl.PropagationFlags]::None
    $allow=[Security.AccessControl.AccessControlType]::Allow
    foreach($entry in @(
        @{sid=$owner;rights=[Security.AccessControl.FileSystemRights]::FullControl},
        @{sid=(New-Object Security.Principal.SecurityIdentifier('S-1-5-18'));rights=[Security.AccessControl.FileSystemRights]::FullControl},
        @{sid=(New-Object Security.Principal.SecurityIdentifier('S-1-15-2-1'));rights=[Security.AccessControl.FileSystemRights]::ReadAndExecute}
    )){
        $rule=New-Object Security.AccessControl.FileSystemAccessRule($entry.sid,$entry.rights,$inherit,$propagation,$allow)
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Directory -AclObject $acl
}
function New-Shortcut([string]$Name,[string]$Target,[string]$Arguments,[int]$WindowStyle=1,[string]$ExpectedExistingHash=''){
    [IO.Directory]::CreateDirectory($script:ShortcutRoot)|Out-Null
    Assert-PlainDirectory $script:ShortcutRoot
    $file=Join-Path $script:ShortcutRoot ($Name+'.lnk')
    $shell=New-Object -ComObject WScript.Shell
    if(Test-Path -LiteralPath $file){
        $current=$shell.CreateShortcut($file)
        if(($current.TargetPath -ne $Target -or $current.Arguments -ne $Arguments) -and -not($ExpectedExistingHash -and (Test-Hash $file $ExpectedExistingHash))){
            $file=Join-Path $script:ShortcutRoot ($Name+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6)+'.lnk')
            Write-Step '同名启动入口指向其他命令，保留原入口并另建插件入口。'
        }
    }
    $shortcut=$shell.CreateShortcut($file);$shortcut.TargetPath=$Target;$shortcut.Arguments=$Arguments
    $shortcut.WorkingDirectory=$script:ManagerRoot;$shortcut.WindowStyle=$WindowStyle;$shortcut.Save()
    return [pscustomobject]@{path=$file;sha256=(Get-FileHash -LiteralPath $file).Hash}
}
function Remove-ManagedShortcut($State){
    if($State.shortcut -and (Test-Path -LiteralPath $State.shortcut.path)){
        Assert-Under $State.shortcut.path $script:ShortcutRoot|Out-Null
        if(Test-Hash $State.shortcut.path $State.shortcut.sha256){Remove-Item -LiteralPath $State.shortcut.path -Force}
        else{Write-Step '启动快捷方式已经被用户修改，保留该文件。'}
    }
}
function Get-LoadedHook([string]$Executable,[string]$Dll){
    foreach($process in @(Get-CimInstance Win32_Process -Filter "Name='cloudmusic.exe' OR Name='AppleMusic.exe' OR Name='AMPLibraryAgent.exe'" -ErrorAction SilentlyContinue)){
        if($process.ExecutablePath -ne $Executable){continue}
        try{
            $module=Get-Process -Id $process.ProcessId | Select-Object -ExpandProperty Modules | Where-Object {$_.FileName -eq $Dll} | Select-Object -First 1
            if($module){return [pscustomobject]@{pid=$process.ProcessId;path=$module.FileName;sha256=(Get-FileHash -LiteralPath $module.FileName).Hash}}
        }catch{Write-Step ('不能读取进程模块，尚未确认实际载入：'+$_.Exception.Message)}
    }
    return $null
}
function Get-NetEaseBridgeState([int]$ProcessId,[datetime]$StartedAfter=[datetime]::MinValue){
    $logPath=Join-Path $env:LOCALAPPDATA ('NCExclusive\plugin-'+$ProcessId+'.jsonl')
    if(-not(Test-Path -LiteralPath $logPath -PathType Leaf)){return 'Waiting'}
    if((Get-Item -LiteralPath $logPath).LastWriteTime -lt $StartedAfter){return 'Waiting'}
    $events=@(Get-Content -LiteralPath $logPath -Encoding utf8 -ErrorAction SilentlyContinue)
    if($events -match '"event":"settings_script_running"'){return 'Ready'}
    if($events -match '"event":"settings_bootstrap_(timeout|failed|schedule_failed)"'){return 'Failed'}
    return 'Waiting'
}
function Wait-NetEaseBridge([int]$ProcessId,[datetime]$StartedAfter=[datetime]::MinValue,[int]$TimeoutSeconds=35){
    $deadline=(Get-Date).AddSeconds($TimeoutSeconds)
    do{
        $bridge=Get-NetEaseBridgeState $ProcessId $StartedAfter
        if($bridge -eq 'Ready'){return}
        if($bridge -eq 'Failed'){break}
        if(-not(Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)){throw ('网易云进程已退出，界面插件未完成启动。日志：'+(Join-Path $env:LOCALAPPDATA ('NCExclusive\plugin-'+$ProcessId+'.jsonl')))}
        Start-Sleep -Milliseconds 200
    }while((Get-Date) -lt $deadline)
    throw ('网易云音频 DLL 已加载，但设置页脚本未确认启动，不能认定独占开关已就绪。日志：'+(Join-Path $env:LOCALAPPDATA ('NCExclusive\plugin-'+$ProcessId+'.jsonl')))
}
function Launch-Application([string]$Name,$Found=$null){
    $state=Get-State $Name
    if(-not $state -or -not $state.enabled){throw '此应用的插件未安装或已取消，请先安装。'}
    if(-not $Found){$Found=Find-Application $Name}
    if($Found.status -ne 'Supported'){throw $Found.reason}
    $install=Get-InstallPath $Name
    foreach($file in $script:Meta.applications.$Name.files){if(-not(Test-Hash (Join-Path $install $file.name) $file.sha256)){throw ('已安装插件校验失败：'+$file.name)}}
    if($Name -eq 'NetEase'){
        $dll=Join-Path $install 'nc-exclusive-hook.dll'
        $loaded=Get-LoadedHook $Found.path $dll
        if(-not $loaded){
            Stop-AppRuntime $Found
            $launcher=Start-Process -FilePath (Join-Path $install 'nc-exclusive-launcher.exe') -ArgumentList ('--launch "'+$Found.path+'"') -WindowStyle Hidden -PassThru
            if(-not $launcher.WaitForExit(20000) -or $launcher.ExitCode -ne 0){throw '网易云插件启动器未成功完成载入'}
            $deadline=(Get-Date).AddSeconds(10)
            do{$loaded=Get-LoadedHook $Found.path $dll;if(-not $loaded){Start-Sleep -Milliseconds 200}}while(-not $loaded -and (Get-Date) -lt $deadline)
        }
        if(-not $loaded){throw '尚未找到已载入网易云插件的主进程'}
        $processStart=(Get-Process -Id $loaded.pid).StartTime
        Wait-NetEaseBridge $loaded.pid $processStart
        # Native single-instance activation restores a hidden or minimized window.
        Start-Process -FilePath $Found.path | Out-Null
        Write-Step ('网易云音频 DLL 已载入，设置页脚本已确认运行，PID='+$loaded.pid+'；进入设置 > 播放可查看独占开关。')
        return $loaded
    }
    Install-Companion
    $coldStart=-not @(Get-AppProcesses $Found | Where-Object Name -EQ 'AppleMusic.exe').Count
    if($coldStart){
        Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -ArgumentList ('shell:AppsFolder\'+$Found.aumid)
    }
    $deadline=(Get-Date).AddSeconds(15)
    do{
        $hostProcesses=@(Get-AppProcesses $Found|Where-Object Name -EQ 'AppleMusic.exe')
        if($hostProcesses.Count){break};Start-Sleep -Milliseconds 200
    }while((Get-Date) -lt $deadline)
    if(-not $hostProcesses.Count){throw '尚未找到 Apple Music 主进程'}
    if($coldStart){
        Write-Step '等待 Apple Music 完成初始加载，再载入伴随插件。'
        $waitDeadline=(Get-Date).AddSeconds(17)
        while((Get-Date) -lt $waitDeadline){
            if(-not @(Get-AppProcesses $Found|Where-Object Name -EQ 'AppleMusic.exe').Count){throw 'Apple Music 已在等待期间退出，未启动伴随插件'}
            Start-Sleep -Milliseconds 250
        }
    }
    if(-not @(Get-AppProcesses $Found|Where-Object Name -EQ 'am-exclusive-broker.exe').Count){
        Start-Process -FilePath (Get-ShellPath) -ArgumentList (Get-AppleHelperArguments) -WindowStyle Hidden
    }
    $loaded=$null;$deadline=(Get-Date).AddSeconds(15)
    do{$loaded=Get-LoadedHook $Found.path (Join-Path $install 'am-exclusive-ui.dll');if(-not $loaded){Start-Sleep -Milliseconds 200}}while(-not $loaded -and (Get-Date) -lt $deadline)
    if(-not $loaded){throw 'Apple Music 界面插件尚未载入'}
    Write-Step ('Apple Music 界面插件实际载入成功，PID='+$loaded.pid+'；音频钩子在播放组件出现后载入。')
    return $loaded
}
function Install-ApplicationCore([string]$Name,$Found){
    $install=Get-InstallPath $Name
    Assert-PlainDirectory $install
    $state=Get-State $Name
    if(-not $state){
        $backup=New-AppBackup $Name $install
        $state=[pscustomobject]@{name=$Name;enabled=$false;target=$Found.path;aumid=$Found.aumid;backup=$backup;disabledFolder='';shortcut=$null;package=$script:SourceRoot;updated=''}
        Save-State $Name $state
    }
    $restoreFolder=$state.disabledFolder
    if($restoreFolder -and (Test-Path -LiteralPath $restoreFolder)){
        Assert-Under $restoreFolder $script:BackupRoot|Out-Null
        if(Test-Path -LiteralPath $install){throw '插件目录被其他文件重新占用，保留两侧文件并停止此应用安装'}
        Move-Item -LiteralPath $restoreFolder -Destination $install
        $state.disabledFolder=''
    }
    $changed=$false
    foreach($file in $script:Meta.applications.$Name.files){if(-not(Test-Hash (Join-Path $install $file.name) $file.sha256)){$changed=$true;break}}
    $wasRunning=@(Get-AppProcesses $Found).Count -gt 0
    if($changed -and $wasRunning){Stop-AppRuntime $Found}
    $newDirectory=-not(Test-Path -LiteralPath $install)
    [IO.Directory]::CreateDirectory($install)|Out-Null
    if($Name -eq 'AppleMusic' -and $newDirectory){
        # Limit a fresh companion folder to the current user and SYSTEM for
        # writes, with package processes allowed to read/execute the DLLs.
        # Existing folder permissions and configuration are preserved.
        Set-NewAppleFolderAcl $install
    }
    foreach($file in $script:Meta.applications.$Name.files){
        $destination=Join-Path $install $file.name
        if(-not(Test-Hash $destination $file.sha256)){
            Copy-Item -LiteralPath (Join-Path $script:SourceRoot ($Name+'/'+$file.name)) -Destination $destination -Force
        }
        if(-not(Test-Hash $destination $file.sha256)){throw ('写入后校验失败：'+$file.name)}
    }
    if($Name -eq 'AppleMusic'){
        $ini=Join-Path $install 'am-exclusive.ini'
        if(-not(Test-Path -LiteralPath $ini)){[IO.File]::WriteAllText($ini,"[mod]`r`nmode=exclusive`r`nfallback_shared=false`r`nperiod_100ns=200000`r`nnative_trace=false`r`n",$script:Utf8)}
        Install-Companion
    }
    $state.enabled=$true;$state.target=$Found.path;$state.aumid=$Found.aumid;$state.package=$script:SourceRoot;$state.updated=(Get-Date).ToString('o')
    $arguments='-NoLogo -NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $script:ManagerRoot 'Manage-AudioExclusive.ps1')+'" -Action Launch -App '+$Name+' -NoMenu'
    $oldShortcutHash=if($state.shortcut){$state.shortcut.sha256}else{''}
    $state.shortcut=New-Shortcut ($script:Labels[$Name]+'（独占插件）') (Get-ShellPath) $arguments 7 $oldShortcutHash
    Save-State $Name $state
    Write-Step ($script:Labels[$Name]+' 插件部署及文件校验完成；配置保留。')
    if($wasRunning){$null=Launch-Application $Name $Found}
    return [pscustomobject]@{app=$Name;result='Installed';target=$Found.path;directory=$install;shortcut=$state.shortcut.path}
}
function Install-Application([string]$Name,$Found){
    $previous=Get-State $Name
    $install=Get-InstallPath $Name
    $hadInstall=Test-Path -LiteralPath $install
    $previousDisabled=''
    if($previous -and $previous.disabledFolder){$previousDisabled=$previous.disabledFolder}
    $backupSource=if($hadInstall){$install}elseif($previousDisabled){$previousDisabled}else{$install}
    $checkpoint=New-AppBackup $Name $backupSource
    if($previous -and $previous.shortcut -and (Test-Path -LiteralPath $previous.shortcut.path)){
        Assert-Under $previous.shortcut.path $script:ShortcutRoot|Out-Null
        Copy-Item -LiteralPath $previous.shortcut.path -Destination (Join-Path $checkpoint 'previous-shortcut.lnk')
    }
    try{return Install-ApplicationCore $Name $Found}
    catch{
        $failure=$_.Exception.Message
        try{
            Write-Step ($script:Labels[$Name]+' 安装未完成，正在恢复本次操作前的插件文件与任务。')
            $restoreAppleTask=($Name -eq 'AppleMusic')
            if($Name -eq 'AppleMusic'){
                $task=Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
                $taskSnapshot=Join-Path $checkpoint 'previous-task.xml'
                if($task -and (Test-Path -LiteralPath $taskSnapshot)){
                    $currentTaskXml=Export-ScheduledTask -TaskName $script:TaskName
                    $restoreAppleTask=($currentTaskXml.Trim() -cne [IO.File]::ReadAllText($taskSnapshot).Trim())
                }elseif(-not $task -and -not(Test-Path -LiteralPath $taskSnapshot)){$restoreAppleTask=$false}
                if($restoreAppleTask -and $task -and (Test-OwnedAppleTask $task)){Disable-ScheduledTask -TaskName $script:TaskName|Out-Null;Stop-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue}
            }
            Stop-AppRuntime $Found
            if(Test-Path -LiteralPath $install){
                Assert-PlainDirectory $install
                $failed=Assert-Under (Join-Path $checkpoint 'failed-deployment') $checkpoint
                Move-Item -LiteralPath $install -Destination $failed
            }
            $copy=Join-Path $checkpoint 'previous-install'
            if(Test-Path -LiteralPath $copy){
                $restore=if($hadInstall){$install}else{$previousDisabled}
                if(-not $restore){throw '恢复目标缺失，快照保留在 '+$checkpoint}
                Assert-PlainDirectory $restore
                Copy-Item -LiteralPath $copy -Destination $restore -Recurse
                $acl=Get-Acl -LiteralPath $restore
                $acl.SetSecurityDescriptorSddlForm([IO.File]::ReadAllText((Join-Path $checkpoint 'previous-install-acl.txt')))
                Set-Acl -LiteralPath $restore -AclObject $acl
            }
            if($restoreAppleTask){
                $xml=Join-Path $checkpoint 'previous-task.xml'
                if(Test-Path -LiteralPath $xml){Register-ScheduledTask -TaskName $script:TaskName -Xml ([IO.File]::ReadAllText($xml)) -Force|Out-Null}
                else{
                    $task=Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
                    if($task -and (Test-OwnedAppleTask $task)){Unregister-ScheduledTask -TaskName $script:TaskName -Confirm:$false}
                }
            }
            $partial=Get-State $Name
            if($partial -and -not $previous){Remove-ManagedShortcut $partial}
            if($previous){
                $oldShortcut=Join-Path $checkpoint 'previous-shortcut.lnk'
                if($previous.shortcut -and (Test-Path -LiteralPath $oldShortcut)){
                    Copy-Item -LiteralPath $oldShortcut -Destination $previous.shortcut.path -Force
                }
                Save-State $Name $previous
            }
            else{Save-State $Name ([pscustomobject]@{name=$Name;enabled=$false;target=$Found.path;aumid=$Found.aumid;backup=$checkpoint;disabledFolder='';shortcut=$null;package=$script:SourceRoot;updated=(Get-Date).ToString('o')})}
            throw ('安装失败，已恢复操作前状态：'+$failure)
        }catch{
            if($_.Exception.Message.StartsWith('安装失败，已恢复操作前状态：')){throw}
            throw ('安装失败：'+$failure+'；自动恢复也未完成：'+$_.Exception.Message+'；恢复快照：'+$checkpoint)
        }
    }
}
function Remove-Application([string]$Name){
    $state=Get-State $Name
    if(-not $state){return [pscustomobject]@{app=$Name;result='NotManaged';reason='统一安装器未管理此应用，无需取消'}}
    if(-not $state.enabled){return [pscustomobject]@{app=$Name;result='AlreadyRemoved';reason='已经取消插入'}}
    $install=Get-InstallPath $Name
    Assert-PlainDirectory $install
    $found=[pscustomobject]@{name=$Name;path=$state.target;aumid=$state.aumid;status='Supported'}
    $companionTaskNote=''
    if($Name -eq 'AppleMusic'){
        $task=Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
        if($task){
            if(-not(Test-OwnedAppleTask $task)){throw '同名计划任务指向其他程序，未修改该任务或取消文件'}
            try{Disable-ScheduledTask -TaskName $script:TaskName|Out-Null}
            catch{$companionTaskNote='计划任务未能禁用：'+$_.Exception.Message+'；插件文件将归档移走，原任务入口不再存在。'}
            try{Stop-ScheduledTask -TaskName $script:TaskName -ErrorAction Stop}
            catch{if(-not $companionTaskNote){$companionTaskNote='计划任务未能停止：'+$_.Exception.Message+'；将停止插件进程并归档文件。'}}
        }
    }
    Stop-AppRuntime $found
    Remove-ManagedShortcut $state
    if(Test-Path -LiteralPath $install){
        $disabled=Join-Path $script:BackupRoot ($Name+'-disabled-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
        Assert-Under $disabled $script:BackupRoot|Out-Null
        [IO.Directory]::CreateDirectory($script:BackupRoot)|Out-Null
        # Both absolute paths were checked above. No recursive deletion: keep
        # plugin files, configuration and logs for reinstall or manual recovery.
        Move-Item -LiteralPath $install -Destination $disabled
        $state.disabledFolder=$disabled
    }
    $state.enabled=$false;$state.updated=(Get-Date).ToString('o');Save-State $Name $state
    Write-Step ($script:Labels[$Name]+' 已取消插入；插件运行已停止，文件和配置已保存在 '+$state.disabledFolder)
    if($companionTaskNote){Write-Step $companionTaskNote}
    return [pscustomobject]@{app=$Name;result='Removed';backup=$state.disabledFolder;companionTaskNote=$companionTaskNote}
}
function Show-ApplicationStatus([string]$Name,$Found){
    $state=Get-State $Name
    $install=Get-InstallPath $Name
    $enabled=$state -and $state.enabled
    $module=$null
    if($Found.path){$module=Get-LoadedHook $Found.path (Join-Path $install $(if($Name -eq 'NetEase'){'nc-exclusive-hook.dll'}else{'am-exclusive-ui.dll'}))}
    $taskEnabled=$false
    if($Name -eq 'AppleMusic'){$task=Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue;if($task){$taskEnabled=[bool]$task.Settings.Enabled}}
    $result=[pscustomobject]@{app=$Name;discovery=$Found.status;managed=[bool]$state;enabled=[bool]$enabled;loaded=[bool]$module;module=$module;taskEnabled=$taskEnabled;target=$Found.path;directory=$install;reason=$Found.reason}
    foreach($field in @('supportedVersions','detectedVersions')){
        if($Found.PSObject.Properties[$field]){$result|Add-Member -NotePropertyName $field -NotePropertyValue $Found.$field}
    }
    return $result
}
function Invoke-ApplicationActions([string]$Operation,[string]$Selection='All'){
    $names=if($Selection -eq 'All'){@('NetEase','AppleMusic')}else{@($Selection)}
    $results=@()
    foreach($name in $names){
        try{
            Write-Step ('处理 '+$script:Labels[$name])
            if($Operation -eq 'Remove'){$results+=Remove-Application $name;continue}
            $found=Find-Application $name
            if($Operation -eq 'Status'){$results+=Show-ApplicationStatus $name $found;continue}
            if($found.status -ne 'Supported'){
                Write-Step ($script:Labels[$name]+'：'+$found.reason+' 已跳过，继续处理其他应用。')
                $skipped=[pscustomobject]@{app=$name;result=$found.status;reason=$found.reason}
                foreach($field in @('supportedVersions','detectedVersions')){
                    if($found.PSObject.Properties[$field]){$skipped|Add-Member -NotePropertyName $field -NotePropertyValue $found.$field}
                }
                $results+=$skipped;continue
            }
            if($Operation -eq 'Install'){$results+=Install-Application $name $found}
            elseif($Operation -eq 'Launch'){$results+=Launch-Application $name $found}
        }catch{
            Write-Host ($script:Labels[$name]+' 处理失败：'+$_.Exception.Message) -ForegroundColor Red
            $results+=[pscustomobject]@{app=$name;result='Failed';reason=$_.Exception.Message}
        }
    }
    return $results
}
function Show-Menu {
    for(;;){
        Write-Host "`n独占插件管理：1 安装/重新插入两款   2 查看状态   3 启动网易云独占版   4 启动 Apple Music"
        Write-Host '              5 取消网易云插入       6 取消 Apple Music 插入       7 取消两款       Q 退出'
        $choice=(Read-Host '输入编号').Trim().ToUpperInvariant()
        $selection='All';$operation=''
        switch($choice){
            '1'{$operation='Install'} '2'{$operation='Status'}
            '3'{$operation='Launch';$selection='NetEase'} '4'{$operation='Launch';$selection='AppleMusic'}
            '5'{$operation='Remove';$selection='NetEase'} '6'{$operation='Remove';$selection='AppleMusic'}
            '7'{$operation='Remove'} 'Q'{return} default{Write-Host '请输入有效编号。';continue}
        }
        $operationMutex=New-Object Threading.Mutex($false,('Local\AudioExclusive.Manager.'+[Security.Principal.WindowsIdentity]::GetCurrent().User.Value))
        $operationLocked=$false
        try{
            try{$operationLocked=$operationMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$operationLocked=$true}
            if(-not $operationLocked){Write-Host '另一个操作正在执行，请稍后再选。';continue}
            $results=@(Invoke-ApplicationActions $operation $selection)
            $results|ConvertTo-Json -Depth 8|Write-Host
            Write-Json (Join-Path $script:ManagerRoot 'last-result.json') @{operation=$operation;time=(Get-Date).ToString('o');results=$results}
        }finally{if($operationLocked){$operationMutex.ReleaseMutex()};$operationMutex.Dispose()}
    }
}
if($LibraryOnly){return}
$runMutex=$null;$locked=$false;$exitCode=0;$transcript=$false
try{
    $runMutex=New-Object Threading.Mutex($false,('Local\AudioExclusive.Manager.'+[Security.Principal.WindowsIdentity]::GetCurrent().User.Value))
    try{$locked=$runMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(-not $locked){throw '另一个独占插件管理窗口正在运行，请先完成或退出该窗口。'}
    [IO.Directory]::CreateDirectory((Join-Path $script:ManagerRoot 'logs'))|Out-Null
    $log=Join-Path $script:ManagerRoot ('logs\manager-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6)+'.txt')
    Start-Transcript -LiteralPath $log -Force|Out-Null;$transcript=$true
    Write-Host "独占插件统一安装器 — 网易云音乐 / Apple Music`n" -ForegroundColor Cyan
    Write-Step ('运行环境：PowerShell '+$PSVersionTable.PSVersion+'；操作日志：'+$log)
    Initialize-Payload
    $manageArguments='-NoLogo -NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $script:ManagerRoot 'Manage-AudioExclusive.ps1')+'" -Action Menu'
    $managementRecord=Join-Path $script:ManagerRoot 'management-shortcut.json'
    $oldManagementHash=''
    if(Test-Path -LiteralPath $managementRecord){$oldManagementHash=(Read-Json $managementRecord).sha256}
    $managementLink=New-Shortcut '独占插件管理（按应用取消）' (Get-ShellPath) $manageArguments 1 $oldManagementHash
    Write-Json $managementRecord $managementLink
    $operation=if($Action -eq 'Auto'){'Install'}else{$Action}
    if($operation -ne 'Menu'){
        $results=@(Invoke-ApplicationActions $operation $App)
        $results|ConvertTo-Json -Depth 8|Write-Host
        Write-Json (Join-Path $script:ManagerRoot 'last-result.json') @{operation=$operation;time=(Get-Date).ToString('o');results=$results}
        if(@($results|Where-Object {$_.PSObject.Properties['result'] -and $_.result -eq 'Failed'}).Count){$exitCode=1}
    }
    Write-Host "`n网易云请使用开始菜单 AudioExclusive 中的独占插件入口。原网易云入口按原生方式启动。"
    Write-Host 'Apple Music 的伴随插件随原应用启动。安装或取消不会修改两款客户端的安装文件。'
    Write-Host '已在运行的应用若需更新或取消插件，会退出该应用；两款播放器不能同时独占同一个设备。'
    if(-not $NoMenu){$runMutex.ReleaseMutex();$locked=$false;Show-Menu}
}catch{Write-Host ('安装器未能完成：'+$_.Exception.Message) -ForegroundColor Red;$exitCode=2;if(-not $NoMenu){Read-Host '按 Enter 关闭'|Out-Null}}
finally{if($transcript){Stop-Transcript|Out-Null};if($locked){$runMutex.ReleaseMutex()};if($runMutex){$runMutex.Dispose()}}
exit $exitCode
