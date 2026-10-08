# Removes VideoBG for the current user: the startup entry, the Start menu shortcut and the VideoBG.exe
# next to this script. Settings and light copies are kept unless -RemoveData.
param([switch]$RemoveData)
$ErrorActionPreference = 'Continue'

$exe = Join-Path $PSScriptRoot 'VideoBG.exe'
# (--exit waits until VideoBG has put your own lock screen and background back and closed.)
$running = Get-CimInstance Win32_Process -Filter "Name='VideoBG.exe'"
if ($running) {
    Start-Process ($running | Select-Object -First 1).ExecutablePath -ArgumentList '--exit' -Wait
    Start-Sleep -Milliseconds 200
}
# If the lock screen or background still shows a video frame (VideoBG crashed or was ended), put yours back.
if (Test-Path $exe) { Start-Process $exe -ArgumentList '--restore-pictures' -Wait }

Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'VideoBG' -ErrorAction SilentlyContinue
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' -Name 'VideoBG' -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'VideoBG.lnk') -ErrorAction SilentlyContinue
Remove-Item $exe -Force -ErrorAction SilentlyContinue

if ($RemoveData) {
    Remove-Item (Join-Path $env:APPDATA 'VideoBG') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $env:LOCALAPPDATA 'VideoBG') -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host 'VideoBG removed, with its settings, light copies and videos made from GIFs.'
} else {
    Write-Host "VideoBG removed. Its settings are still in $env:APPDATA\VideoBG (delete that folder if you won't reinstall)."
}
