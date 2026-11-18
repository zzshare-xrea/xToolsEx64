# xToolsEx64 / xToolsEx86 installer for the SmartRename (xTools) context menu.
# Run by double-clicking the bat file, or right-click -> Run as administrator.
#
# Why HKLM: Explorer merges HKCU\Software\Classes into HKCR, but many third-party
# file managers (Directory Opus, Everything, Total Commander plugins, ...) only
# query HKLM\Software\Classes. So the extension is registered machine-wide here,
# plus the Wow6432Node mirror so 32-bit programs can load xToolsEx86.dll.
#
# This file is PURE ASCII on purpose (PowerShell 5.1 decodes UTF-8 without BOM as
# the ANSI codepage, which garbles Chinese text and can even eat closing quotes).
# Registry access goes through reg.exe, which treats "*" as a literal key name.

param(
    [string]$ExePath = '',
    [switch]$Elevated
)

$ErrorActionPreference = 'Continue'
$Clsid    = '{9E7B4C21-3A55-4F0E-8D1A-2B3C4D5E6F70}'
$Targets  = @('*', 'Directory', 'Folder', 'Drive', 'Directory\Background')
$Reg      = Join-Path $env:SystemRoot 'System32\reg.exe'

function Info($m) { Write-Host $m -ForegroundColor Cyan }
function Ok($m)   { Write-Host $m -ForegroundColor Green }
function Warn($m) { Write-Host $m -ForegroundColor Yellow }
function Err($m)  { Write-Host $m -ForegroundColor Red }

$MenuText = -join @(0x62D6, 0x628A, 0x66F4, 0x540D, 0x5668 | ForEach-Object { [char]$_ })
$HelpText = -join @(0x6DFB, 0x52A0, 0x5230, 0x20, 0x62D6, 0x628A, 0x66F4, 0x540D, 0x5668,
                    0x7684, 0x6587, 0x4EF6, 0x5217, 0x8868 | ForEach-Object { [char]$_ })

function RegExists([string]$key) {
    & $Reg query $key /ve > $null 2>&1
    return ($LASTEXITCODE -eq 0)
}

function RegDefault([string]$key) {
    $lines = @(& $Reg query $key /ve 2>$null)
    if ($LASTEXITCODE -ne 0) { return $null }
    foreach ($l in $lines) {
        if ($l -match 'REG_SZ\s+(.+)$') { return $Matches[1].Trim() }
        if ($l -match 'REG_EXPAND_SZ\s+(.+)$') { return $Matches[1].Trim() }
    }
    return $null
}

# ---------------------------------------------------------------- admin + 64-bit
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    if (-not $Elevated) {
        Warn 'Elevation required (writing to HKLM). Asking for administrator rights...'
        $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'), '-Elevated')
        if ($ExePath) { $argv += @('-ExePath', ('"' + $ExePath + '"')) }
        Start-Process -FilePath $ps -ArgumentList $argv -Verb RunAs
        exit
    }
}

if ([IntPtr]::Size -ne 8) {
    $sys = Join-Path $env:SystemRoot 'sysnative\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $sys) {
        $argv2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'), '-Elevated')
        Start-Process -FilePath $sys -ArgumentList $argv2 -Wait
    } else { Err 'Please run this with 64-bit PowerShell.' }
    return
}

Info '== xTools context menu installer (64-bit + 32-bit) =='
Write-Host ''

# ---------------------------------------------------------------- locate DLLs
$Dll64 = Join-Path $PSScriptRoot 'xToolsEx64.dll'
$Dll32 = Join-Path $PSScriptRoot 'xToolsEx86.dll'
if (-not (Test-Path -LiteralPath $Dll64)) {
    Err 'xToolsEx64.dll not found next to this script.'
    exit 1
}
$Dll64 = (Resolve-Path -LiteralPath $Dll64).Path
$have32 = Test-Path -LiteralPath $Dll32
if ($have32) { $Dll32 = (Resolve-Path -LiteralPath $Dll32).Path }
Info ('64-bit DLL: ' + $Dll64)
if ($have32) { Info ('32-bit DLL: ' + $Dll32) } else { Warn 'No xToolsEx86.dll; 32-bit programs (Everything 1.4) will not see the menu.' }

# ---------------------------------------------------------------- locate xTools.exe
function Find-XTools([string]$hint) {
    $cands = New-Object System.Collections.Generic.List[string]
    if ($hint) { $cands.Add($hint) }
    $cands.Add((Join-Path $PSScriptRoot 'xTools.exe'))
    $up = Split-Path -Parent $PSScriptRoot
    if ($up) { $cands.Add((Join-Path $up 'xTools.exe')) }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA, $env:APPDATA, $env:USERPROFILE)) {
        if ($root) {
            $cands.Add((Join-Path $root 'xTools\xTools.exe'))
            $cands.Add((Join-Path $root 'SmartRename\xTools.exe'))
        }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

$Exe = Find-XTools $ExePath
while (-not $Exe) {
    Warn 'xTools.exe was not found automatically.'
    $inputPath = Read-Host 'Drag xTools.exe into this window or paste its full path (blank = abort)'
    if ([string]::IsNullOrWhiteSpace($inputPath)) { Err 'Cancelled.'; exit 1 }
    $inputPath = $inputPath.Trim().Trim([char]34)
    if (Test-Path -LiteralPath $inputPath -PathType Leaf) { $Exe = (Resolve-Path -LiteralPath $inputPath).Path }
    else { Warn ('Path does not exist: ' + $inputPath) }
}
Info ('Program: ' + $Exe)
Write-Host ''

# ---------------------------------------------------------------- register DLLs
Info 'Registering COM servers...'
$regsvr64 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
$p1 = Start-Process -FilePath $regsvr64 -ArgumentList @('/s', ('"' + $Dll64 + '"')) -Wait -PassThru
if ($p1.ExitCode -ne 0) { Err ('regsvr32 (64-bit) returned ' + $p1.ExitCode); exit 1 }
Ok '64-bit component registered.'
# DllRegisterServer reports non-fatal issues (for example stale HKCU entries it could
# not remove) through HKCU\Software\xTools\ShellEx64\LastWarn. Empty means all clean.
$lastWarn = ''
try { $lastWarn = [string](Get-ItemProperty -LiteralPath 'HKCU:\Software\xTools\ShellEx64' -Name 'LastWarn' -ErrorAction Stop).LastWarn } catch { }
if ($lastWarn) {
    Warn ('Registration warning: ' + $lastWarn)
    Warn 'If it mentions HKCU stale registration, an old copy may still be loaded; restart Explorer after cleaning it.'
}
if ($have32) {
    $regsvr32exe = Join-Path $env:SystemRoot 'SysWOW64\regsvr32.exe'
    if (Test-Path -LiteralPath $regsvr32exe) {
        $p2 = Start-Process -FilePath $regsvr32exe -ArgumentList @('/s', ('"' + $Dll32 + '"')) -Wait -PassThru
        if ($p2.ExitCode -eq 0) { Ok '32-bit component registered.' }
        else { Warn ('regsvr32 (32-bit) returned ' + $p2.ExitCode) }
    }
}

# ---------------------------------------------------------------- clean old entries
Info 'Cleaning old registration (HKCU leftovers + old 32-bit entries)...'
$cleaned = 0

# 1. leftovers that regsvr32 just wrote into HKCU (we want everything in HKLM)
foreach ($t in $Targets) {
    $k = 'HKCU\Software\Classes\' + $t + '\shellex\ContextMenuHandlers\xToolsEx64'
    & $Reg query $k /ve > $null 2>&1
    if ($LASTEXITCODE -eq 0) { & $Reg delete $k /f > $null 2>&1 }
}
foreach ($t in $Targets) {
    $k = 'HKCU\Software\Classes\' + $t + '\shellex\ContextMenuHandlers\xToolsEx86'
    & $Reg query $k /ve > $null 2>&1
    if ($LASTEXITCODE -eq 0) { & $Reg delete $k /f > $null 2>&1 }
}
foreach ($k in @('HKCU\Software\Classes\CLSID\' + $Clsid, 'HKCU\Software\Classes\Wow6432Node\CLSID\' + $Clsid)) {
    & $Reg query $k /ve > $null 2>&1
    if ($LASTEXITCODE -eq 0) { & $Reg delete $k /f > $null 2>&1; $cleaned++ }
}

# 2. the original 32-bit xToolsEx.dll handler, if any manager still points at it
$OldNames = @('xToolsExtension', 'xToolsEx')
foreach ($base in @('HKLM\Software\Classes', 'HKLM\Software\Classes\Wow6432Node', 'HKCU\Software\Classes')) {
    foreach ($t in $Targets) {
        $hbase = $base + '\' + $t + '\shellex\ContextMenuHandlers'
        $names = @()
        foreach ($n in $OldNames) { $names += $n }
        $lines = @(& $Reg query $hbase 2>$null)
        if ($LASTEXITCODE -eq 0) {
            foreach ($l in $lines) {
                $leaf = ($l.Trim() -split '\\')[-1]
                if ($leaf -and ($names -notcontains $leaf)) { $names += $leaf }
            }
        }
        foreach ($n in $names) {
            $hk = $hbase + '\' + $n
            $guid = RegDefault $hk
            if (-not $guid) { continue }
            $dllv = RegDefault ($base + '\CLSID\' + $guid + '\InprocServer32')
            if (-not $dllv) { continue }
            if ($dllv -match 'xToolsEx(H|64|86)?\.dll') {
                & $Reg delete $hk /f > $null 2>&1
                if ($LASTEXITCODE -eq 0) { $cleaned++; Warn ('Removed old handler: ' + $hk) }
            }
        }
    }
}
if ($cleaned -eq 0) { Ok 'Nothing to clean.' }

# ---------------------------------------------------------------- register machine-wide
Info 'Registering in HKLM (visible to all file managers)...'

# 64-bit view
$cls64 = 'HKLM\Software\Classes\CLSID\' + $Clsid
& $Reg add ($cls64 + '\InprocServer32') /ve /t REG_SZ /d ('"' + $Dll64 + '"') /f > $null 2>&1
& $Reg add ($cls64 + '\InprocServer32') /v ThreadingModel /t REG_SZ /d Apartment /f > $null 2>&1
foreach ($t in $Targets) {
    $k = 'HKLM\Software\Classes\' + $t + '\shellex\ContextMenuHandlers\xToolsEx64'
    & $Reg add $k /ve /t REG_SZ /d $Clsid /f > $null 2>&1
}

# 32-bit view (Wow6432Node) for Everything 1.4, 32-bit Total Commander, etc.
if ($have32) {
    $cls32 = 'HKLM\Software\Classes\Wow6432Node\CLSID\' + $Clsid
    & $Reg add ($cls32 + '\InprocServer32') /ve /t REG_SZ /d ('"' + $Dll32 + '"') /f > $null 2>&1
    & $Reg add ($cls32 + '\InprocServer32') /v ThreadingModel /t REG_SZ /d Apartment /f > $null 2>&1
    foreach ($t in $Targets) {
        $k = 'HKLM\Software\Classes\Wow6432Node\' + $t + '\shellex\ContextMenuHandlers\xToolsEx86'
        & $Reg add $k /ve /t REG_SZ /d $Clsid /f > $null 2>&1
    }
}

# ---------------------------------------------------------------- verify
$verifyKey = 'HKLM\Software\Classes\*\shellex\ContextMenuHandlers\xToolsEx64'
$guid = RegDefault $verifyKey
if (-not $guid) { Err ('Verification failed: ' + $verifyKey + ' has no CLSID'); exit 1 }
if ((RegDefault ($cls64 + '\InprocServer32')) -eq $null) { Err 'Verification failed: InprocServer32 missing'; exit 1 }
Ok 'HKLM registration verified.'

# ---------------------------------------------------------------- config (per user)
Info 'Writing per-user configuration...'
$kind = [Microsoft.Win32.RegistryValueKind]::String
$ck = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\xTools\ShellEx64')
$ck.SetValue('ExePath', $Exe, $kind)
if (-not $ck.GetValue('MenuText')) { $ck.SetValue('MenuText', $MenuText, $kind) }
if (-not $ck.GetValue('HelpText')) { $ck.SetValue('HelpText', $HelpText, $kind) }
if (-not $ck.GetValue('Method'))   { $ck.SetValue('Method', 'both', $kind) }
$ck.Close()
Ok 'Configuration written.'

Write-Host ''
Ok '== Installation complete =='
Write-Host 'Right-click a file in Explorer, Everything or Directory Opus.'
Write-Host ''
Warn 'Directory Opus: enable shell extension menu items in its preferences'
Warn '  (Settings > Preferences, look for "shell extension" / "Explorer context menu").'
Warn 'Everything 1.4 is a 32-bit program: with xToolsEx86.dll present it can show the menu.'
Write-Host ''
try { $ans = Read-Host 'Restart Explorer now? (Y/N)' } catch { $ans = 'N' }
if ($ans -match '^[Yy]') {
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Ok 'Explorer restarted.'
}
