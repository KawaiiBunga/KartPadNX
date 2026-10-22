#!/usr/bin/env python3
"""Generate a private Switch game graph with the staged KartPad translator."""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent
REF = ROOT / "ref/kartpad-main"
sys.path.insert(0, str(REF / "builder"))
from kartpad_builder.retro_rewind import validate_pack, validate_rwfc_payload
from kartpad_builder.profiles import sha256_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=["online", "retro-rewind"], default="retro-rewind")
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--payload-provenance", type=Path,
                        help="Explicit metadata for a newer production-signed payload")
    parser.add_argument("--retro-root", type=Path)
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=6)
    args = parser.parse_args()
    config = json.loads((REF / "builder/profiles/mkwii-rmcp01-rev0.json").read_text())
    disc = ROOT / "private/self-build/disc"
    expected = {"sys/main.dol": "80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05",
                "files/rel/StaticR.rel": "16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d"}
    for name, digest in expected.items():
        if sha256_file(disc / name) != digest:
            raise ValueError("Unsupported disc: " + name)
    payload_config = config["retroRewind"]["payload"]
    if args.payload_provenance:
        payload_config = json.loads(args.payload_provenance.read_text())
    validate_rwfc_payload(args.payload, payload_config)  # Always checks production RSA key.
    staged = ROOT / "build/wiicompiled-fpscr"
    translator = staged / "translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
    if not translator.is_file():
        raise FileNotFoundError("Run the base translator preparation first")
    work = ROOT / "private/self-build" / args.profile
    output = work / "translation"
    functions = output / "functions"
    metadata = output / "base_translation_output.json"
    base_dir = output / "base"
    mod = work / "mod"
    shards = output / "build_shards"
    base_dir.mkdir(parents=True, exist_ok=True)
    if args.profile == "retro-rewind":
        if not args.retro_root:
            parser.error("--retro-root is required for retro-rewind")
        root = args.retro_root.resolve()
        validate_pack(root, config["retroRewind"])
        code = root / "Binaries/Code.pul"
        staged_code = work / "input/Binaries/Code.pul"
        staged_code.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(code, staged_code)
    else:
        root = work / "empty-mod"
        root.mkdir(parents=True, exist_ok=True)
        code = work / "empty-code.pul"
        code.write_bytes(bytes.fromhex((REF / "tools/empty-kamek-v3.hex").read_text()))
    manifest = ROOT / "build" / ("mkwii-rmcp01-" + args.profile + ".yml")
    shutil.copy2(REF / "tools" / manifest.name, manifest)
    common = ["--project", manifest, "--profile", args.profile]
    def run(*command):
        subprocess.run([str(x) for x in command], cwd=ROOT, check=True)
    def translate(*command):
        run("dotnet", translator, *command)
    translate("translate-recursive", "0x800060A4", *common, "--threads", args.jobs,
              "--prune-stale", "--output-metadata", metadata)
    run(sys.executable, HERE / "translator-post-hooks.py", "functions", functions)
    translate("emit-base-manifest", *common, "--out", base_dir, "--functions-dir", functions,
              "--translation-output-metadata", metadata, "--region", "P")
    translate("translate-mod", *common, "--base-manifest", base_dir / "mkwii_base_manifest.json",
              "--base-translation-output-metadata", metadata, "--code-pul", code,
              "--mod-root", root, "--mod-name", args.profile, "--region", "P", "--out", mod,
              "--prefer-cached-inputs", "--emit-cpp", "--threads", args.jobs,
              "--retro-wfc-payload", args.payload.resolve())
    translate("generate-data-init", *common)
    translate("emit-build-shards", *common, "--base-metadata", metadata,
              "--base-functions-dir", functions, "--native-source-dir", staged / "runtime/src",
              "--resolved-profile", mod / "resolved_dispatch_profile.json",
              "--retro-cpp-dir", mod / "cpp", "--out", shards)
    run(sys.executable, HERE / "translator-post-hooks.py", "shards", shards)
    if "set(MKW_HAVE_RETRO_REWIND_SHARDS ON)" not in (shards / "shards.cmake").read_text():
        raise ValueError("Missing translated mod graph")
    (work / "payload-provenance.json").write_text(json.dumps(payload_config, indent=2))
    print("Generated private graph:", shards, flush=True)

if __name__ == "__main__":
    main()
