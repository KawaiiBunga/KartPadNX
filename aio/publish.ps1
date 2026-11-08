# Publishes a new builder release in one go. The web builder switches to it by itself within
# 10 minutes, and players' PC builders can download it from GitHub Releases.
#
#   powershell -ExecutionPolicy Bypass -File aio\publish.ps1 0.2.0
#   powershell -ExecutionPolicy Bypass -File aio\publish.ps1 0.2.0 -Notes "Retro Rewind 6.13.5, mods tab"
#
# It: checks your changes are committed, pushes main, builds the launcher (launcher\build-native.ps1,
# no Docker), builds the release (aio\release.ps1, about 20 minutes), and creates the GitHub release
# with KartPadNX-Builder.zip attached, tagged at the commit it pushed. Releases are marked
# pre-release unless you pass -Stable.
#
# One-time setup: install the GitHub CLI and log in:
#   winget install GitHub.cli
#   gh auth login
#
# Still needs a Docker source build first (aio\release.ps1 -BuildRoot, docs/BUILDING-SWITCH.md) when
# the runtime, Aurora, Dawn or the renderer shaders changed; release.ps1 stops and says so.
param([Parameter(Mandatory = $true)][string]$Version, [string]$Notes = "", [switch]$Stable)
$ErrorActionPreference = "Stop"
$aio = $PSScriptRoot
$repo = Split-Path -Parent $aio
Set-Location $repo

function Need($ok, $message) { if (-not $ok) { Write-Host "`n$message" -ForegroundColor Red; exit 1 } }
function Run($exe) { & $exe @args; if ($LASTEXITCODE) { throw "$exe $($args -join ' ') failed (exit $LASTEXITCODE)" } }

# Tools: the GitHub CLI, and git (on PATH, or the one GitHub Desktop ships).
$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh -and (Test-Path "$env:ProgramFiles\GitHub CLI\gh.exe")) { $gh = "$env:ProgramFiles\GitHub CLI\gh.exe" }
Need $gh "The GitHub CLI isn't installed. Run these once, then try again:`n  winget install GitHub.cli`n  gh auth login"
& $gh auth status *> $null
Need ($LASTEXITCODE -eq 0) "The GitHub CLI isn't logged in. Run this once, then try again:`n  gh auth login"
$git = (Get-Command git -ErrorAction SilentlyContinue).Source
if (-not $git) {
    $git = Get-ChildItem "$env:LOCALAPPDATA\GitHubDesktop\app-*\resources\app\git\cmd\git.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
Need $git "git not found (install Git or GitHub Desktop)."

# The release's tag must hold exactly the code it was built from (the web builder reads its scripts there).
Need ((& $git rev-parse --abbrev-ref HEAD) -eq "main") "Switch to the main branch first."
Need (-not (& $git status --porcelain --untracked-files=no)) "You have uncommitted changes. Commit them in GitHub Desktop, then run this again."
& $gh release view $Version *> $null
Need ($LASTEXITCODE -ne 0) "A release called $Version already exists. Pick a new version number."

Write-Host "`n== push main" -ForegroundColor Cyan
Run $gh auth setup-git
Run $git push origin main
$commit = (& $git rev-parse HEAD).Trim()

Write-Host "`n== launcher" -ForegroundColor Cyan
$launcher = Join-Path $aio "out\launcher"
Run powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "launcher\build-native.ps1") -Out $launcher

Write-Host "`n== release build (about 20 minutes)" -ForegroundColor Cyan
Run powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $aio "release.ps1") -Launcher $launcher
$zip = Join-Path $aio "out\KartPadNX-Builder.zip"

Write-Host "`n== GitHub release $Version" -ForegroundColor Cyan
if (-not $Notes) { $Notes = "KartPadNX builder $Version. Download KartPadNX-Builder.zip, or build on the website." }
$releaseArgs = @("release", "create", $Version, $zip, "--target", $commit, "--title", $Version, "--notes", $Notes)
if (-not $Stable) { $releaseArgs += "--prerelease" }
Run $gh @releaseArgs
Write-Host "`nPublished $Version. The web builder switches to it within 10 minutes." -ForegroundColor Green
