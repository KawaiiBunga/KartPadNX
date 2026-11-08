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
the page says the builder is offline.

## Setup (once)

1. Have a working AIO setup on this PC (`aio\out\python`, `aio\out\dist`, `aio\out\obj\nxshim.exe`;
   run `aio\release.ps1` once if they're missing). About 80 GB free for the work folder: the shared
   build cache, plus ~6.5 GB per finished build (kept 24 hours) and the uploads in progress.
2. Start the server:

   ```powershell
   aio\out\python\python.exe web\server\build_server.py --work D:\kpnx-web `
     --origin https://kawaiibunga.github.io --warm-disc iso\RMCP01.wbfs
   ```

   `--warm-disc` makes it build with your disc whenever Retro Rewind's feed has a new version, so
   the first player after an RR update doesn't wait an hour.
3. Give it a public HTTPS address. Easiest is a Cloudflare tunnel (free, no port forwarding):

   ```powershell
   cloudflared tunnel --url http://localhost:8787
   ```

   It prints a `https://<random>.trycloudflare.com` address. That address changes every time
   cloudflared restarts; for a fixed one, set up a named tunnel on a domain you own.
4. Put that address in `web/site/config.js` (`window.KPNX_SERVER = 'https://...';`), commit and
   push.
5. Turn on Pages once: GitHub repo > Settings > Pages > Source: **GitHub Actions**. The workflow
   (`.github/workflows/pages.yml`) publishes `web/site` on every push that changes it. Pages on a
   free account needs the repo to be public.

The link to share is `https://kawaiibunga.github.io/KartPadNX/`. To try a different server without
pushing, add `?server=https://...` to that link.

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
