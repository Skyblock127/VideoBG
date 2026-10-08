# Installs VideoBG for the current user (no admin rights needed). The folder this script is in is
# the app's home:
#   * in the project, the built dist\VideoBG.exe is copied here first
#   * from the zip (Install.cmd), the VideoBG.exe next to this script is used where it is
# Then it adds a Start menu shortcut and the startup entry (turn that on/off in Settings > Apps >
# Startup; VideoBG never changes it itself) and starts VideoBG.
param([switch]$NoStartup)
$ErrorActionPreference = 'Stop'

$dir = $PSScriptRoot
$exe = Join-Path $dir 'VideoBG.exe'
$built = Join-Path $dir 'dist\VideoBG.exe'
$project = Test-Path $built
if (-not $project -and -not (Test-Path $exe)) { throw 'No VideoBG.exe here. In the project, build first: .\build.ps1' }

$downloads = (New-Object -ComObject Shell.Application).Namespace('shell:Downloads').Self.Path
if ($downloads -and $dir -like "$downloads*") {
    Write-Warning "VideoBG will run from $dir. If you clear out Downloads later, move this folder somewhere permanent first and run Install.cmd again."
}

# Close any running copy so the exe can be replaced.
# (--exit waits until VideoBG has put your own lock screen and background back and closed.)
$running = Get-CimInstance Win32_Process -Filter "Name='VideoBG.exe'"
if ($running) {
    Start-Process ($running | Select-Object -First 1).ExecutablePath -ArgumentList '--exit' -Wait
    Start-Sleep -Milliseconds 200
}
# The exe can stay locked for a moment after VideoBG has closed: wait for its processes, then retry.
Get-Process VideoBG -ErrorAction SilentlyContinue | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
if ($project) {
    for ($i = 0; ; $i++) {
        try { Copy-Item $built $exe -Force; break }
        catch { if ($i -ge 25) { throw } ; Start-Sleep -Milliseconds 200 }
    }
}

$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Programs')) 'VideoBG.lnk'))
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = $dir
$lnk.Description = 'VideoBG - video wallpaper and desktop clock'
$lnk.IconLocation = "$exe,0"
$lnk.Save()

if (-not $NoStartup) {
    Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'VideoBG' -Value "`"$exe`" --startup"
}

# A reinstall from the project starts quietly in the tray; a first install opens the settings.
if ($project) { Start-Process $exe -ArgumentList '--startup' } else { Start-Process $exe }
Write-Host "Installed: $exe"
Write-Host "Start menu: VideoBG   |   Toggle: Ctrl+Alt+B   |   Startup: Settings > Apps > Startup"
