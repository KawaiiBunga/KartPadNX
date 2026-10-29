# Builds the KartPad-NX AIO builder with the C# compiler that ships with Windows.
#
#   powershell -ExecutionPolicy Bypass -File aio\build.ps1 [-Bundle aio\out\bundle.zip] [-Single]
#
# The pipeline scripts, the tool manifest and nxshim.exe are embedded in the exe. With -Bundle
# (aio\toolkit\make-bundle.py: Python, the build tools and the toolkit), a first run downloads
# only devkitPro:
#   aio\out\dist\KartPadNX-Builder.exe + KartPadNX-Builder.data, zipped together as
#   aio\out\KartPadNX-Builder.zip, the release. The exe starts in about a second.
#   -Single also writes aio\out\dist-single\KartPadNX-Builder.exe with the bundle appended: one
#   file, but Windows Defender re-scans all of it on every launch (~10 s before the window
#   shows, ~40 s the first time).
# Without -Bundle the exe downloads every tool and looks for KartPadNX-Toolkit*.zip next to it.
param([string]$Bundle = "", [switch]$Single)
$ErrorActionPreference = "Stop"
$aio = $PSScriptRoot
$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc (.NET Framework 4.x)" }
$obj = Join-Path $aio "out\obj"
$dist = Join-Path $aio "out\dist"
New-Item -ItemType Directory -Force $obj, $dist | Out-Null

# nxshim.exe is rebuilt only when its source changes: csc's output differs on every build, and Windows
# app control judges each new unsigned binary afresh (on 2026-10-28 a rebuilt shim of unchanged
# source was blocked as the compiler front end while the previous build of it ran). The source's
# hash is kept next to the binary.
$shim = Join-Path $obj "nxshim.exe"
$shimSrc = Join-Path $aio "shim\nxshim.cs"
$shimStamp = "$shim.source-sha256"
$srcHash = (Get-FileHash $shimSrc -Algorithm SHA256).Hash
if (-not (Test-Path $shim) -or -not (Test-Path $shimStamp) -or (Get-Content $shimStamp -Raw).Trim() -ne $srcHash) {
  & $csc /nologo /target:exe /optimize+ /out:$shim $shimSrc
  if ($LASTEXITCODE) { throw "nxshim build failed" }
  Set-Content -Path $shimStamp -Value $srcHash -Encoding ascii
}

$front = Join-Path $obj "KartPadNX-Builder.exe"
$res = @(
  "/resource:$(Join-Path $aio 'pipeline\kpnx_aio.py'),kpnx_aio.py",
  "/resource:$(Join-Path $aio 'pipeline\nxtools.py'),nxtools.py",
  "/resource:$(Join-Path $aio 'manifest\tools.json'),tools.json",
  "/resource:$shim,nxshim.exe"
)
& $csc /nologo /target:winexe /optimize+ /out:$front `
  /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll `
  /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll `
  @res (Join-Path $aio "frontend\Builder.cs")
if ($LASTEXITCODE) { throw "frontend build failed" }

function Join-Files([string]$Out, [string[]]$Parts) {
  $o = [IO.File]::Create($Out)
  try { foreach ($p in $Parts) { $i = [IO.File]::OpenRead($p); try { $i.CopyTo($o, 1MB) } finally { $i.Dispose() } } }
  finally { $o.Dispose() }
}

Get-ChildItem $dist -Filter "KartPadNX-Toolkit*.zip" | Remove-Item
Copy-Item $front (Join-Path $dist "KartPadNX-Builder.exe") -Force
$data = Join-Path $dist "KartPadNX-Builder.data"
if ($Bundle) {
  $Bundle = (Resolve-Path $Bundle).Path
  Copy-Item $Bundle $data -Force
  $release = Join-Path $aio "out\KartPadNX-Builder.zip"
  if (Test-Path $release) { Remove-Item $release }
  Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
  $zip = [IO.Compression.ZipFile]::Open($release, 'Create')
  try {
    # The .data file is already compressed; store it.
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $dist "KartPadNX-Builder.exe"), "KartPadNX-Builder/KartPadNX-Builder.exe", 'Optimal')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $data, "KartPadNX-Builder/KartPadNX-Builder.data", 'NoCompression')
  } finally { $zip.Dispose() }
  if ($Single) {
    $one = Join-Path $aio "out\dist-single"
    New-Item -ItemType Directory -Force $one | Out-Null
    Join-Files (Join-Path $one "KartPadNX-Builder.exe") @($front, $Bundle)
  }
} elseif (Test-Path $data) { Remove-Item $data }
Get-ChildItem $dist, (Join-Path $aio "out") -File | Where-Object { $_.Name -like 'KartPadNX-Builder*' } |
  ForEach-Object { "{0,-50} {1,14:N0} bytes" -f $_.FullName.Substring($aio.Length + 1), $_.Length }
