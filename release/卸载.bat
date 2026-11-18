@echo off
cd /d "%~dp0"
echo Removing xToolsEx64 shell extension...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall-xToolsEx64.ps1"
echo.
pause
