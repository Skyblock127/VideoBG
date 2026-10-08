# Publishes a GitHub release of the current commit: builds dist\VideoBG-<version>.zip and uploads it as
# release v<version>, the version being APP_VERSION in src\common.h (keep res\app.rc in step).
# The README's download link opens the newest release.
# Needs the GitHub CLI (gh), signed in. -Notes takes a Markdown file with what's new; the install /
# update steps are added below it.
param([string]$Notes = '')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Select-String -Path "$root\src\common.h" -Pattern 'APP_VERSION\s+L"([0-9.]+)"').Matches[0].Groups[1].Value
$tag = "v$version"
if (git -C $root status --porcelain) { throw 'Commit your changes first.' }
if (git -C $root tag -l $tag) { throw "$tag already exists: raise APP_VERSION in src\common.h and res\app.rc first." }

& powershell -NoProfile -File "$root\build.ps1" -Zip
if ($LASTEXITCODE) { throw 'build failed' }
git -C $root push origin HEAD
if ($LASTEXITCODE) { throw 'push failed' }

$gh = @($tag, "$root\dist\VideoBG-$version.zip", '--title', "VideoBG $version", '--target', (git -C $root rev-parse HEAD))
# Every release ends with how to install or update, below the notes.
$howTo = @'

### Install or update
Download the zip below. It holds a **VideoBG** folder.
- **New to VideoBG:** right-click the zip, choose **Extract All**, and extract it somewhere it can stay, for example `C:\Tools` (you get `C:\Tools\VideoBG`). Then run **Install.cmd** in that folder.
- **Updating:** right-click the VideoBG tray icon and choose **Exit**. Extract the zip to the same place as before (the folder that holds your VideoBG folder, for example `C:\Tools`) and choose **Replace the files**, then run **Install.cmd** again. No need to uninstall; your settings are kept.
'@
$text = if ($Notes) { (Get-Content -Raw $Notes).TrimEnd() + "`r`n" } else { '' }
$notesFile = "$root\build\release-notes.md"
[IO.File]::WriteAllText($notesFile, $text + $howTo)  # UTF-8 without a BOM
$gh += @('--notes-file', $notesFile)
gh release create @gh
if ($LASTEXITCODE) { throw 'release failed' }
git -C $root fetch --tags --quiet
