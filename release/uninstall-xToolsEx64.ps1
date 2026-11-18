# xTools context menu uninstaller (64-bit + 32-bit, HKLM + HKCU).
# PURE ASCII on purpose. Run as administrator.

param([switch]$Elevated)

$ErrorActionPreference = 'Continue'
$Clsid   = '{9E7B4C21-3A55-4F0E-8D1A-2B3C4D5E6F70}'
$Targets = @('*', 'Directory', 'Folder', 'Drive', 'Directory\Background')
$Reg     = Join-Path $env:SystemRoot 'System32\reg.exe'

function Info($m) { Write-Host $m -ForegroundColor Cyan }
function Ok($m)   { Write-Host $m -ForegroundColor Green }
function Warn($m) { Write-Host $m -ForegroundColor Yellow }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    if (-not $Elevated) {
        Warn 'Asking for administrator rights (HKLM cleanup)...'
        $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        Start-Process -FilePath $ps -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'), '-Elevated') -Verb RunAs
        exit
    }
}

Info '== xTools context menu uninstaller =='
Write-Host ''

$Dll64 = Join-Path $PSScriptRoot 'xToolsEx64.dll'
$Dll32 = Join-Path $PSScriptRoot 'xToolsEx86.dll'
if (Test-Path -LiteralPath $Dll64) {
    $regsvr64 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
    $p = Start-Process -FilePath $regsvr64 -ArgumentList @('/s', '/u', ('"' + (Resolve-Path -LiteralPath $Dll64).Path + '"')) -Wait -PassThru
    if ($p.ExitCode -eq 0) { Ok '64-bit DLL unregistered.' } else { Warn ('regsvr32 /u returned ' + $p.ExitCode) }
}
if (Test-Path -LiteralPath $Dll32) {
    $regsvr32exe = Join-Path $env:SystemRoot 'SysWOW64\regsvr32.exe'
    if (Test-Path -LiteralPath $regsvr32exe) {
        $p2 = Start-Process -FilePath $regsvr32exe -ArgumentList @('/s', '/u', ('"' + (Resolve-Path -LiteralPath $Dll32).Path + '"')) -Wait -PassThru
        if ($p2.ExitCode -eq 0) { Ok '32-bit DLL unregistered.' } else { Warn ('regsvr32 /u (32-bit) returned ' + $p2.ExitCode) }
    }
}

Info 'Cleaning registry...'
$bases = @('HKLM\Software\Classes', 'HKLM\Software\Classes\Wow6432Node', 'HKCU\Software\Classes', 'HKCU\Software\Classes\Wow6432Node')
foreach ($base in $bases) {
    foreach ($t in $Targets) {
        foreach ($name in @('xToolsEx64', 'xToolsEx86')) {
            $k = $base + '\' + $t + '\shellex\ContextMenuHandlers\' + $name
            & $Reg query $k /ve > $null 2>&1
            if ($LASTEXITCODE -eq 0) {
                & $Reg delete $k /f > $null 2>&1
                if ($LASTEXITCODE -eq 0) { Warn ('Removed ' + $k) }
            }
        }
    }
}
foreach ($base in @('HKLM\Software\Classes\CLSID', 'HKLM\Software\Classes\Wow6432Node\CLSID', 'HKCU\Software\Classes\CLSID', 'HKCU\Software\Classes\Wow6432Node\CLSID')) {
    $k = $base + '\' + $Clsid
    & $Reg query $k /ve > $null 2>&1
    if ($LASTEXITCODE -eq 0) {
        & $Reg delete $k /f > $null 2>&1
        if ($LASTEXITCODE -eq 0) { Warn ('Removed ' + $k) }
    }
}
$cfg = 'HKCU\Software\xTools\ShellEx64'
& $Reg query $cfg /ve > $null 2>&1
if ($LASTEXITCODE -eq 0) { & $Reg delete $cfg /f > $null 2>&1; Ok 'Configuration removed.' }

Write-Host ''
Ok '== Uninstall complete =='
try { $ans = Read-Host 'Restart Explorer now? (Y/N)' } catch { $ans = 'N' }
if ($ans -match '^[Yy]') { Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue; Ok 'Explorer restarted.' }
