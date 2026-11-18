@echo off
cd /d "%~dp0"
echo Installing xToolsEx64 shell extension (64-bit)...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-xToolsEx64.ps1"
echo.
pause
