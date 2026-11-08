# Web builder

Players without a fast PC build on yours: the site checks their disc, uploads it, your PC runs the
normal AIO pipeline, and they download `mkw-launcher.nro`.

| Part | Where it runs |
|---|---|
| `site/` (static HTML/JS) | GitHub Pages, free (`.github/workflows/pages.yml`) |
| `server/build_server.py` | your PC, Python stdlib only, behind a tunnel |

## Setup (once)

1. Have a working AIO setup on this PC (`aio\out\python`, `aio\out\dist`, `aio\out\obj\nxshim.exe`;
   run `aio\release.ps1` once if they're missing). ~60 GB free for the work folder.
2. Start the server:

   ```powershell
   aio\out\python\python.exe web\server\build_server.py --work D:\kpnx-web `
     --origin https://kawaiibunga.github.io --warm-disc iso\RMCP01.wbfs
   ```

   `--warm-disc` makes it build with your disc whenever Retro Rewind's feed has a new version, so
   the first player after an RR update doesn't wait an hour.
3. Expose it over HTTPS. Easiest is a Cloudflare tunnel (free, no port forwarding):

   ```powershell
   cloudflared tunnel --url http://localhost:8787
   ```

   For a URL that doesn't change on restart, use a named tunnel on your own domain.
4. Put that URL in `web/site/config.js`, push, and turn on Pages (Settings > Pages > Source:
   GitHub Actions). The site is at `https://kawaiibunga.github.io/KartPadNX/`.

## How it behaves

- Uploads go in 32 MiB chunks (Cloudflare caps a request at 100 MB) and resume after a dropped
  connection or a page reload.
- The site checks the disc header locally (RMCP01, revision 0, ISO/WBFS/RVZ); the server checks
  it again, and the pipeline checks the exact `main.dol` and `StaticR.rel` hashes.
- One build at a time, up to 6 jobs uploading or waiting. Translation and compiled games are
  shared between builds (every accepted disc has the same game code), so after the first build a
  player's build only re-reads their disc and packs the launcher.
- Uploads are deleted when their build ends, results after 24 hours, and nothing survives a
  restart.
- `python web/server/test_build_server.py` runs the whole flow against a fake pipeline.

## Before you open it up

The NRO you hand back contains translated Nintendo code. `docs/DISTRIBUTION.md` says game NROs
are never distributed; serving one back to the person who uploaded the disc is still distribution
from your machine. The ownership checkbox and disc check don't change that. Decide if you're OK
with it before sharing the link.
