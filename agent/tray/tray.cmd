@echo off
rem RemoteBuilder agent tray monitor launcher
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tray.ps1" %*
