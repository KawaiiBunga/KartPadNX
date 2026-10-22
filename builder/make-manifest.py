#!/usr/bin/env python3
"""
Generate manifest.json (schema 1) for MKW Launcher release.
"""

import argparse
import datetime
import hashlib
import json
import os
import sys


def main():
    parser = argparse.ArgumentParser(description="Generate manifest.json for MKW Launcher release")
    parser.add_argument("launcher_nro", help="Path to mkw-launcher.nro")
    parser.add_argument("version", help="Release version (e.g. 1.0.0 or v1.0.0)")
    parser.add_argument("--channel", choices=["stable", "beta"], default="stable",
                        help="Release channel (default: stable)")
    parser.add_argument("--changelog", default=None,
                        help="Path to changelog text file or raw changelog string")
    parser.add_argument("--tag", default=None,
                        help="GitHub release tag (default: derived from version)")
    parser.add_argument("--output", "-o", default="manifest.json",
                        help="Output path for manifest.json (default: manifest.json)")

    args = parser.parse_args()

    if not os.path.isfile(args.launcher_nro):
        sys.stderr.write(f"ERROR: Launcher NRO file not found: {args.launcher_nro}\n")
        sys.exit(1)

    with open(args.launcher_nro, "rb") as f:
        data = f.read()

    file_size = len(data)
    file_sha256 = hashlib.sha256(data).hexdigest()

    version_str = args.version.lstrip("v")
    tag = args.tag or (f"v{version_str}" if not args.version.startswith("v") else args.version)

    changelog_text = f"MKW Launcher {tag}"
    if args.changelog:
        if os.path.isfile(args.changelog):
            with open(args.changelog, "r", encoding="utf-8") as f:
                changelog_text = f.read().strip()
        else:
            changelog_text = args.changelog.strip()

    now_utc = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    manifest = {
        "schema": 1,
        "version": version_str,
        "channel": args.channel,
        "released": now_utc,
        "changelog": changelog_text,
        "components": [
            {
                "id": "launcher",
                "file": "mkw-launcher.nro",
                "size": file_size,
                "sha256": file_sha256,
                "url": f"https://github.com/KawaiiBunga/Kartpad-NX/releases/download/{tag}/mkw-launcher.nro"
            }
        ]
    }

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"==> Generated {args.output}:")
    print(f"    Version: {version_str} (tag: {tag})")
    print(f"    Channel: {args.channel}")
    print(f"    Size: {file_size:,} bytes")
    print(f"    SHA-256: {file_sha256}")
    print(f"    URL: {manifest['components'][0]['url']}")


if __name__ == "__main__":
    main()
