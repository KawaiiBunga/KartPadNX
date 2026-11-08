# Opens the web builder on this PC: starts the build server and a free Cloudflare tunnel, and
# publishes the tunnel's address so https://kawaiibunga.github.io/KartPadNX/ finds it.
# Close this window (or press Ctrl+C) to stop; the site then shows "Closed".
#
#   double-click web\start-builder.cmd      (or: powershell -ExecutionPolicy Bypass -File web\start-builder.ps1)
#
# Options: -Work DIR (default private\web-builder, needs ~80 GB), -WarmDisc FILE (default iso\RMCP01.wbfs:
# prebuilds each new release / Retro Rewind with your disc so players don't wait).
param([string]$Work = "", [string]$WarmDisc = "", [string]$Repo = "KawaiiBunga/KartPadNX")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Work) { $Work = Join-Path $root "private\web-builder" }
if (-not $WarmDisc -and (Test-Path (Join-Path $root "iso\RMCP01.wbfs"))) { $WarmDisc = Join-Path $root "iso\RMCP01.wbfs" }
New-Item -ItemType Directory -Force $Work | Out-Null

function Find($name, [string[]]$paths) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $paths | Where-Object { Test-Path $_ } | Select-Object -First 1
}
$py = Join-Path $root "aio\out\python\python.exe"
if (-not (Test-Path $py)) { throw "$py is missing: run aio\release.ps1 once." }
$gh = Find gh @("$env:ProgramFiles\GitHub CLI\gh.exe")
if (-not $gh) { throw "The GitHub CLI is missing: winget install GitHub.cli, then gh auth login" }
$cf = Find cloudflared @("${env:ProgramFiles(x86)}\cloudflared\cloudflared.exe", "$env:ProgramFiles\cloudflared\cloudflared.exe")
if (-not $cf) {
    Write-Host "Installing cloudflared (the free tunnel)..." -ForegroundColor Cyan
    winget install --id Cloudflare.cloudflared -e --accept-source-agreements --accept-package-agreements | Out-Host
    $cf = Find cloudflared @("${env:ProgramFiles(x86)}\cloudflared\cloudflared.exe", "$env:ProgramFiles\cloudflared\cloudflared.exe")
    if (-not $cf) { throw "cloudflared didn't install; run: winget install Cloudflare.cloudflared" }
}

# The address the site reads: server.txt on the builder-url branch (via the GitHub API, so nothing
# is committed to main and this checkout is untouched).
function Publish($text) {
    $ErrorActionPreference = "Continue"
    & $gh api "repos/$Repo/git/ref/heads/builder-url" 2>&1 | Out-Null
    if ($LASTEXITCODE) {
        $main = (& $gh api "repos/$Repo/git/ref/heads/main" --jq .object.sha).Trim()
        & $gh api -X POST "repos/$Repo/git/refs" -f "ref=refs/heads/builder-url" -f "sha=$main" | Out-Null
    }
    $sha = (& $gh api "repos/$Repo/contents/server.txt?ref=builder-url" --jq .sha 2>$null)
    $body = @("-X", "PUT", "repos/$Repo/contents/server.txt", "-f", "message=Web builder address", "-f", "branch=builder-url",
              "-f", "content=$([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("$text`n")))")
    if ($LASTEXITCODE -eq 0 -and $sha) { $body += @("-f", "sha=$($sha.Trim())") }
    & $gh api @body | Out-Null
    if ($LASTEXITCODE) { Write-Host "Could not publish the address to GitHub (gh auth login?)" -ForegroundColor Red }
}

$serverLog = Join-Path $Work "server.log"
$tunnelLog = Join-Path $Work "tunnel.log"
$serverArgs = @("-u", "`"$root\web\server\build_server.py`"", "--work", "`"$Work`"", "--origin", "https://kawaiibunga.github.io", "--repo", $Repo)
if ($WarmDisc) { $serverArgs += @("--warm-disc", "`"$WarmDisc`"") }
$server = $null
$tunnel = $null
try {
    Write-Host "Starting the build server (first start downloads the newest release, ~300 MB)..." -ForegroundColor Cyan
    $server = Start-Process $py $serverArgs -NoNewWindow -PassThru -RedirectStandardOutput $serverLog -RedirectStandardError "$serverLog.err"
    $deadline = (Get-Date).AddMinutes(15)
    while (-not (Select-String -Path $serverLog -Pattern "web builder on" -Quiet -ErrorAction SilentlyContinue)) {
        if ($server.HasExited) { throw "The build server stopped. See $serverLog and $serverLog.err" }
        if ((Get-Date) -gt $deadline) { throw "The build server didn't start in 15 minutes. See $serverLog" }
        Start-Sleep 2
    }

    Write-Host "Starting the tunnel..." -ForegroundColor Cyan
    $tunnel = Start-Process $cf @("tunnel", "--no-autoupdate", "--url", "http://localhost:8787") -NoNewWindow -PassThru `
        -RedirectStandardOutput "$tunnelLog.out" -RedirectStandardError $tunnelLog
    $url = $null
    $deadline = (Get-Date).AddMinutes(2)
    while (-not $url) {
        if ($tunnel.HasExited) { throw "The tunnel stopped. See $tunnelLog" }
        if ((Get-Date) -gt $deadline) { throw "The tunnel didn't give an address in 2 minutes. See $tunnelLog" }
        Start-Sleep 2
        $m = Select-String -Path $tunnelLog -Pattern "https://[a-z0-9-]+\.trycloudflare\.com" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($m) { $url = $m.Matches[0].Value }
    }
    Publish $url
    Write-Host ""
    Write-Host "The web builder is OPEN: https://kawaiibunga.github.io/KartPadNX/" -ForegroundColor Green
    Write-Host "  server address $url (the site picks it up within a few minutes)"
    Write-Host "  logs in $Work"
    Write-Host "Leave this window open. Close it or press Ctrl+C to stop."
    while (-not $server.HasExited -and -not $tunnel.HasExited) { Start-Sleep 5 }
    Write-Host "Something stopped; see $serverLog and $tunnelLog" -ForegroundColor Red
} finally {
    foreach ($p in @($tunnel, $server)) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
    Publish "closed"
    Write-Host "The web builder is closed."
}
