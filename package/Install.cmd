@echo off
rem Sets VideoBG up from this folder: Start menu shortcut, start with Windows, and starts it.
if not exist "%~dp0VideoBG.exe" (
  echo Extract the zip first: right-click it ^> Extract All, put the folder where it can stay,
  echo then run Install.cmd from there.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if errorlevel 1 (pause) else (timeout /t 5)
