@echo off
rem Double-clickable wrapper for install_shortcut.ps1 (a .ps1 opens in an editor
rem when double-clicked, and the default execution policy blocks it besides).
rem Arguments are passed through:  install_shortcut.cmd -RemoteHost user@server
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_shortcut.ps1" %*
echo.
pause
