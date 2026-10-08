# Builds dist\VideoBG.exe with MinGW-w64 (g++ / windres on PATH). -Zip also packs dist\VideoBG-<version>.zip, the
# download for someone else: the exe, How to use.txt, Install.cmd / Uninstall.cmd and the font licenses.
param([switch]$Debug, [switch]$Zip)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$noManifest = Join-Path $build 'nomanifest'
New-Item -ItemType Directory -Force $build, (Join-Path $root 'dist'), $noManifest | Out-Null

if (-not (Test-Path "$root\res\app.ico")) { python "$root\tools\make_icons.py" }

& windres -I "$root\src" "$root\res\app.rc" -O coff -o "$build\app.res.o"
if ($LASTEXITCODE) { throw 'windres failed' }

# MinGW links its own default manifest; shadow it with an empty object so ours (DPI awareness,
# segment heap, ...) is the only one in the exe.
Set-Content -Encoding ascii "$noManifest\empty.c" ''
& gcc -c "$noManifest\empty.c" -o "$noManifest\default-manifest.o"
if ($LASTEXITCODE) { throw 'gcc failed' }

$opt = if ($Debug) { @('-O1', '-g', '-fno-omit-frame-pointer') } else { @('-O2', '-s', '-flto') }
$flags = @('-std=c++20', '-municode', '-mwindows', '-static', '-fno-exceptions', '-fno-rtti', '-fno-devirtualize',
           '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000008',
           '-Wall', '-Wno-unknown-pragmas', '-Wno-missing-field-initializers', '-Wno-stringop-overread')
$libs = @('-lole32', '-loleaut32', '-luuid', '-lmfuuid', '-lshell32', '-luser32', '-lgdi32', '-ladvapi32',
          '-ldwmapi', '-lwtsapi32', '-lpsapi', '-lshlwapi', '-lwindowscodecs', '-lpropsys')
$src = Get-ChildItem "$root\src\*.cpp" | ForEach-Object FullName

# A running copy of the dist build keeps the exe locked; ask it to quit first.
$out = "$root\dist\VideoBG.exe"
if (Test-Path $out) {
    $running = Get-CimInstance Win32_Process -Filter "Name='VideoBG.exe'" | Where-Object ExecutablePath -eq $out
    if ($running) { Start-Process $out -ArgumentList '--exit' -Wait; Start-Sleep -Milliseconds 200 }
}

& g++ "-B$noManifest\" @opt @flags @src "$build\app.res.o" -o "$root\dist\VideoBG.exe" @libs
if ($LASTEXITCODE) { throw 'compile failed' }
$size = [math]::Round((Get-Item "$root\dist\VideoBG.exe").Length / 1KB)
Write-Host "Built dist\VideoBG.exe ($size KB)"

if ($Zip) {
    # Everything goes in one VideoBG folder inside the zip: extracting it where an earlier copy was
    # extracted (the folder holding VideoBG) updates that copy, whatever the zip is called.
    $version = (Select-String -Path "$root\src\common.h" -Pattern 'APP_VERSION\s+L"([0-9.]+)"').Matches[0].Groups[1].Value
    $stage = Join-Path $build 'package'
    $pkg = Join-Path $stage 'VideoBG'
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $pkg | Out-Null
    Copy-Item "$root\dist\VideoBG.exe", "$root\install.ps1", "$root\uninstall.ps1", "$root\package\*" $pkg
    Copy-Item "$root\res\fonts\OFL.txt" "$pkg\Font license (OFL).txt"
    Copy-Item "$root\res\fonts\Apache-2.0.txt" "$pkg\Font license (Apache).txt"
    $zipFile = "$root\dist\VideoBG-$version.zip"
    Remove-Item "$root\dist\VideoBG*.zip" -ErrorAction SilentlyContinue
    Compress-Archive $pkg $zipFile -Force
    $zsize = [math]::Round((Get-Item $zipFile).Length / 1KB)
    Write-Host "Packed dist\VideoBG-$version.zip ($zsize KB)"
}
