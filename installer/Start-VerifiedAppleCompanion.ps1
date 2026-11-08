[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$nativeModules=Join-Path $PSHOME 'Modules'
$windowsModules=Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules'
$env:PSModulePath=(@($nativeModules,$windowsModules)+@($env:PSModulePath -split ';')|Select-Object -Unique)-join ';'
$expected=[IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'AMExclusive'))
if([IO.Path]::GetFullPath($PSScriptRoot) -ne $expected){throw 'Unexpected companion directory'}
$log=Join-Path $PSScriptRoot 'verified-startup.log'
try{
    $package=Get-AppxPackage -Name AppleInc.AppleMusicWin -ErrorAction SilentlyContinue|Sort-Object Version -Descending|Select-Object -First 1
    if(-not $package){return}
    $profiles=@(
        @{'AppleMusic.exe'='50D9F6393A8578B7DDFB6F7E550BB8D2B2A10C32E55AC159D3463A3ECA775227';'AMPLibraryAgent.exe'='4AD22991186710051E872FDCDBAB91980A55802BE75F931044399A38D0CB5C30';'CoreMedia.dll'='505AD3A5A804C1A00A2F58A8237292F000EFACB0EE57850E5B6B3716ADF155CE'}
        @{'AppleMusic.exe'='269B4676C2C91767F0305BB72AB4661AE5BBDDB58B5EF6B489D4DCBC69151000';'AMPLibraryAgent.exe'='1FB27B064AE71BBF71EC7E99209DD1B9640E9F042CE6201269E994181AFAE0C4';'CoreMedia.dll'='40736BC27AE049408E3A54EC40D7279DF002E69282DD729C09C725BE34E0ED4A';'CoreAudioToolbox.dll'='9D4061B8F482B7F2548079A96BC3B5B30B23F2AEBB4D5DF444331BA90FE886D8';'CoreFoundation.dll'='A31EE0F1BD1FFD64442D0E001AA506BB8198E18EA87C9A484EFF8F749BC4D1DE';'AVFoundationCF.dll'='808509507B6D72D594D9B43E3FD35988116A5AEB3C89423104673B265FB57DAA'}
        @{'AppleMusic.exe'='FF03FB24B4C15A38A787AE90A40AC1A0D83FDE0E793E6E8957D7C7B5F77F9E51';'AMPLibraryAgent.exe'='D79B2562251DB7A72E2B44C202EB61A220B10AAEDA085295CEE3EB0EBA54B6F5';'CoreMedia.dll'='40736BC27AE049408E3A54EC40D7279DF002E69282DD729C09C725BE34E0ED4A';'CoreAudioToolbox.dll'='9D4061B8F482B7F2548079A96BC3B5B30B23F2AEBB4D5DF444331BA90FE886D8';'CoreFoundation.dll'='A31EE0F1BD1FFD64442D0E001AA506BB8198E18EA87C9A484EFF8F749BC4D1DE';'AVFoundationCF.dll'='808509507B6D72D594D9B43E3FD35988116A5AEB3C89423104673B265FB57DAA'}
    )
    $matched=$false
    $failed=@()
    foreach($fingerprints in $profiles){
        $profileMatches=$true
        foreach($name in $fingerprints.Keys){
            $file=Join-Path $package.InstallLocation $name
            if(-not(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $fingerprints[$name]){
                $profileMatches=$false
                $failed+=($name+' 文件缺失或 SHA256 不匹配')
                break
            }
        }
        if($profileMatches){$matched=$true;break}
    }
    if(-not $matched){
        $versions=@('软件包版本 '+[string]$package.Version)
        foreach($clientFile in @('AppleMusic.exe','AMPLibraryAgent.exe')){
            $clientPath=Join-Path $package.InstallLocation $clientFile
            $version='文件不存在'
            if(Test-Path -LiteralPath $clientPath -PathType Leaf){$version=[Diagnostics.FileVersionInfo]::GetVersionInfo($clientPath).FileVersion}
            $versions+=($clientFile+' 文件版本 '+$version)
        }
        $reason='Apple Music：当前版本不支持。已支持版本：软件包 1.1540.23042.0 x64（AppleMusic.exe / AMPLibraryAgent.exe 文件版本 1.6.4.90）、软件包 1.1792.39682.0 x64（AppleMusic.exe / AMPLibraryAgent.exe 文件版本 1.7.0.155）、软件包 1.1792.39942.0 x64（文件版本 1.7.0.156）。当前探测到的版本：'+($versions -join '；')+'。原因：'+(($failed|Select-Object -Unique) -join '；')+'；版本号相同也必须通过文件指纹校验。已跳过注入。路径：'+$package.InstallLocation
        Add-Content -LiteralPath $log -Encoding utf8 -Value ((Get-Date -Format o)+' '+$reason)
        return
    }
    $payload=@{
        'am-exclusive-broker.exe'='E0677CC5AD6955B5109C33EF4173572E29B97B92786EA7A8E58813CD6543EBD3'
        'am-exclusive-hook.dll'='329C64AA75C7B2715DAB01A2FD2BA513F676D75349872505F49F5890BEFAE34D'
        'am-exclusive-ui.dll'='C394089108D6FB3402D25DAB1C2E7D35F659D3AC315A9C94DA25A1BF19644D18'
    }
    foreach($name in $payload.Keys){
        $file=Join-Path $PSScriptRoot $name
        if(-not(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $payload[$name]){throw ('Companion payload changed: '+$name)}
    }
    $hostPath=Join-Path $package.InstallLocation 'AppleMusic.exe'
    if(-not @(Get-CimInstance Win32_Process -Filter "Name='AppleMusic.exe'"|Where-Object ExecutablePath -EQ $hostPath).Count){return}
    Add-Content -LiteralPath $log -Encoding utf8 -Value ((Get-Date -Format o)+' fingerprints verified; starting PID-bound companion')
    $companion=Start-Process -FilePath (Join-Path $PSScriptRoot 'am-exclusive-broker.exe') -WindowStyle Hidden -PassThru
    $companion.WaitForExit()
}catch{Add-Content -LiteralPath $log -Encoding utf8 -Value ((Get-Date -Format o)+' startup refused: '+$_.Exception.Message);exit 1}
