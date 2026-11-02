@echo off
rem Installer: asks for your decrypted disc (.iso or folder) and a language. See README.md.
rem You can also drag a .iso or a folder onto this file.
setlocal
cd /d "%~dp0"
if not "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install.ps1" -Source "%~1"
  pause
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\install.ps1"
