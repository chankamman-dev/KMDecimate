@echo off
rem KM Decimate uninstaller - runs install.ps1 -Uninstall next to this file
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
