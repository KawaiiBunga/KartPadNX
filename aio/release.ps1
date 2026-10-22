# Builds a KartPadNX builder release. Everything it makes or caches stays in aio\out (gitignored).
#
#   powershell -ExecutionPolicy Bypass -File aio\release.ps1                     # from the last release
#   powershell -ExecutionPolicy Bypass -File aio\release.ps1 -From X.data       # from a given release's .data
#   powershell -ExecutionPolicy Bypass -File aio\release.ps1 -BuildRoot DIR     # after a Docker source build
#
# By default the binary parts (Dawn/SDL, the staged runtime, translator, launcher, shaders, PGO,
# seeds) are taken from the previous release in aio\out\dist, and everything else from this repo,
# so nothing outside the repo is needed. Use -BuildRoot only when the runtime, Aurora, Dawn, the
# launcher or the renderer shaders changed and were rebuilt with Docker (docs/BUILDING-SWITCH.md).
#
# Output: aio\out\KartPadNX-Builder.zip (the release) and aio\out\dist\. Takes about 20 minutes,
# mostly compiling the game-independent code (aio\out\work keeps those builds for next time).
param([string]$From = "", [string]$BuildRoot = "", [switch]$Single)
$ErrorActionPreference = "Stop"
$aio = $PSScriptRoot
$out = Join-Path $aio "out"
New-Item -ItemType Directory -Force $out | Out-Null

if (-not $From -and -not $BuildRoot) {
  $From = Join-Path $out "dist\KartPadNX-Builder.data"
  if (-not (Test-Path $From)) { throw "No previous release in aio\out\dist. Pass -From <KartPadNX-Builder.data> or -BuildRoot <Docker build checkout>." }
}
if ($From) {
  # The build below replaces aio\out\dist, so work from a copy.
  $prev = Join-Path $out "previous-release.data"
  if ((Resolve-Path $From).Path -ne $prev) { Copy-Item $From $prev -Force }
  $From = $prev
}

# Python to run the toolkit scripts: the previous release's, or the pinned embeddable download.
$py = Join-Path $out "python\python.exe"
if (-not (Test-Path $py)) {
  Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
  $dir = Join-Path $out "python"
  New-Item -ItemType Directory -Force $dir | Out-Null
  $zip = $null
  if ($From) {
    $z = [IO.Compression.ZipFile]::OpenRead($From)
    try {
      foreach ($e in $z.Entries) {
        if ($e.FullName.StartsWith("python/") -and -not $e.FullName.EndsWith("/")) {
          $dest = Join-Path $dir $e.FullName.Substring(7)
          New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
          [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
        }
      }
    } finally { $z.Dispose() }
  }
  if (-not (Test-Path $py)) {
    $spec = (Get-Content (Join-Path $aio "manifest\tools.json") -Raw | ConvertFrom-Json).python
    $zip = Join-Path $out "python-embed.zip"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest $spec.url -OutFile $zip -UseBasicParsing
    if ((Get-FileHash $zip).Hash.ToLower() -ne $spec.sha256) { throw "Python download is corrupt" }
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $dir)
    Remove-Item $zip
  }
}

function Step($name, [scriptblock]$body) {
  Write-Host "`n== $name" -ForegroundColor Cyan
  $t = Get-Date
  & $body
  if ($LASTEXITCODE) { throw "$name failed (exit $LASTEXITCODE)" }
  Write-Host ("   {0:N0} s" -f ((Get-Date) - $t).TotalSeconds)
}

$toolkitArgs = @("--out", (Join-Path $out "toolkit"), "--prebuilt-data", (Join-Path $out "work"))
if ($From) { $toolkitArgs += @("--from-release", $From, "--bundle", $From) }
else { $toolkitArgs += @("--build-root", $BuildRoot) }

Step "toolkit (sources from the repo, prebuilt game-independent code)" {
  & $py (Join-Path $aio "toolkit\make-toolkit.py") @toolkitArgs
}
Step "bundle (Python, build tools, toolkit)" {
  & $py (Join-Path $aio "toolkit\make-bundle.py") --toolkit (Join-Path $out "toolkit") `
    --out (Join-Path $out "bundle.zip") --cache (Join-Path $out "cache")
}
Step "exe and release zip" {
  $buildArgs = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $aio "build.ps1"), "-Bundle", (Join-Path $out "bundle.zip"))
  if ($Single) { $buildArgs += "-Single" }
  & powershell @buildArgs
}
Write-Host "`nRelease: $(Join-Path $out 'KartPadNX-Builder.zip')" -ForegroundColor Green
