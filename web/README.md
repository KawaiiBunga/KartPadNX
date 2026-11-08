# Web builder

Players without a fast PC build on yours: the site checks their disc and uploads it, your PC runs
the normal AIO pipeline, and they download one zip with their whole SD card folder (launcher with
both games, their game files, the matching `RetroRewind6`). They unzip it to the SD card and play,
the same result as the PC builder.

| Part | Where it runs |
|---|---|
| `site/` (static HTML/JS) | GitHub Pages, free: `https://kawaiibunga.github.io/KartPadNX/` |
| `server/build_server.py` | your PC, Python stdlib only, reached through a tunnel |

The page alone does nothing; it talks to the server on your PC. When your PC or the server is off,
the page says it's closed. It always shows the newest Retro Rewind (and its release date), the
builder release builds use, and when the repo's code last changed.

## Where the build code comes from

Builds always use the **newest GitHub release** of this repo (pre-releases count, drafts don't):
its `KartPadNX-Builder.zip` (toolkit, prebuilt parts, Python) plus the pipeline scripts at that
release's tag (`aio/pipeline/kpnx_aio.py`, `nxtools.py`, `aio/manifest/tools.json`). The server
checks for a new release every 10 minutes and downloads it once; a build already running finishes
on the release it started with. So to update what the site builds, commit your changes in GitHub
Desktop and run:

```powershell
powershell -ExecutionPolicy Bypass -File aio\publish.ps1 0.2.0
```

It pushes, builds the launcher and the release, and publishes it on GitHub (one-time setup:
`winget install GitHub.cli`, then `gh auth login`). The first build after a new release takes the
full time (about an hour); `--warm-disc` does that one for you.

Retro Rewind itself comes from Retro Rewind's own feed at build time, if the release's pipeline
has `rr_from_feed` (releases after 0.1.0-Beta); older ones use the approved list.

`--pipeline aio\pipeline\kpnx_aio.py` builds from this checkout and its `aio\out` instead (for
testing changes before a release).

## Opening and closing it

**Open:** double-click `web\start-builder.cmd`. It starts the build server, starts a free
Cloudflare tunnel (installs `cloudflared` the first time), and publishes the tunnel's address to
the `builder-url` branch, where the site reads it. **Close:** close that window (or Ctrl+C); the
site then shows "Closed".

Needs: `aio\out\python` and `aio\out\obj\nxshim.exe` (run `aio\release.ps1` once), the GitHub CLI
logged in (`gh auth login`), and about 80 GB free in the work folder (`private\web-builder`: the
shared build cache, ~6.5 GB per finished build for 24 hours, uploads in progress). With
`iso\RMCP01.wbfs` present it prebuilds each new release and Retro Rewind with your disc, so the
first player after an update doesn't wait an hour. Logs: `private\web-builder\server.log` and
`tunnel.log`.

Once: GitHub repo > Settings > Pages > Source: **GitHub Actions** (the workflow
`.github/workflows/pages.yml` publishes `web/site` on every push that changes it).

The link to share is `https://kawaiibunga.github.io/KartPadNX/`. `?server=https://...` on it tries
another server.

## Your IP address

Players never connect to your PC: the page comes from GitHub and every request goes through
Cloudflare's tunnel, which your PC reaches with outgoing connections only. The server listens on
`127.0.0.1` only, so nothing on your network or the internet can reach it directly. Everything it
sends back passes `scrub()` in `build_server.py`, which removes local paths and IP addresses from
build logs and error messages, and its `Server` header names no Python version. The `builder-url`
branch holds only the `trycloudflare.com` address. Your PC's own address is still seen by the sites
it downloads from (GitHub, Retro Rewind's CDN), as with any download.

## How it behaves

- Uploads go in 32 MiB chunks (Cloudflare caps a request at 100 MB) and resume after a dropped
  connection or a page reload.
- The site checks the disc header locally (RMCP01, revision 0, ISO/WBFS/RVZ); the server checks
  it again, and the pipeline checks the exact `main.dol` and `StaticR.rel` hashes.
- One build at a time, up to 6 jobs uploading or waiting. Translation and compiled games are
  shared between builds (every accepted disc has the same game code), so after the first build a
  player's build only reads their disc, copies its files and packs: minutes, not an hour.
- The download is streamed as a zip while it's sent (no second copy on disk), through a one-time
  link that expires after 10 minutes.
- Uploads are deleted when their build ends, finished builds after 24 hours, and nothing survives
  a restart.
- `python web/server/test_build_server.py` runs the whole flow against a fake pipeline.

## Before you open it up

The zip you hand back contains translated Nintendo code and the files from the player's disc.
`docs/DISTRIBUTION.md` says game builds and disc data are never distributed; sending them back to
the person who uploaded the disc is still distribution from your machine, and every accepted disc
produces the same files. The ownership checkbox and disc check don't change that. Decide if you're
OK with it before sharing the link.
