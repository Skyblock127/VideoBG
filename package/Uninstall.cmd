@echo off
rem Removes VideoBG: startup entry, Start menu shortcut and VideoBG.exe in this folder.
choice /c yn /n /m "Also delete VideoBG's settings and light copies? [Y/N] "
if errorlevel 2 (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" -RemoveData
)
pause
