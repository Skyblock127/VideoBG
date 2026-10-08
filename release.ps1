# Publishes a GitHub release of the current commit: builds dist\VideoBG.zip and uploads it as
# release v<version>, the version being APP_VERSION in src\common.h (keep res\app.rc in step).
# The README's download link always points to the newest release's VideoBG.zip.
# Needs the GitHub CLI (gh), signed in. -Notes takes a Markdown file for the release text.
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

$gh = @($tag, "$root\dist\VideoBG.zip", '--title', "VideoBG $version", '--target', (git -C $root rev-parse HEAD))
if ($Notes) { $gh += @('--notes-file', $Notes) } else { $gh += '--generate-notes' }
gh release create @gh
if ($LASTEXITCODE) { throw 'release failed' }
git -C $root fetch --tags --quiet
