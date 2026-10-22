#!/usr/bin/env python3
"""
Compress KartPad game NROs with zstd (level 19, long-distance matching) and generate payloads.json.
Staged into build/launcher/romfs/payload/ for embedding into the launcher NRO RomFS.
"""

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
import re
import sys
import time


def get_zstd_compressor():
    # 1. Try python-zstandard if available
    try:
        import zstandard as zstd
        def compress_zstandard(data: bytes, level: int = 19, ldm: bool = True) -> bytes:
            params = zstd.ZstdCompressionParameters.from_level(level, enable_ldm=ldm)
            cctx = zstd.ZstdCompressor(compression_params=params)
            return cctx.compress(data)
        return compress_zstandard
    except ImportError:
        pass

    # 2. Try loading libzstd via ctypes
    lib_path = ctypes.util.find_library('zstd') or '/usr/lib/x86_64-linux-gnu/libzstd.so.1'
    try:
        lib = ctypes.CDLL(lib_path)
        lib.ZSTD_createCCtx.restype = ctypes.c_void_p
        lib.ZSTD_freeCCtx.argtypes = [ctypes.c_void_p]
        lib.ZSTD_freeCCtx.restype = ctypes.c_size_t

        lib.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
        lib.ZSTD_compressBound.restype = ctypes.c_size_t

        lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
        lib.ZSTD_isError.restype = ctypes.c_uint

        lib.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        lib.ZSTD_getErrorName.restype = ctypes.c_char_p

        lib.ZSTD_CCtx_setParameter.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        lib.ZSTD_CCtx_setParameter.restype = ctypes.c_size_t

        lib.ZSTD_compress2.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        lib.ZSTD_compress2.restype = ctypes.c_size_t

        ZSTD_c_compressionLevel = 100
        ZSTD_c_enableLongDistanceMatching = 160
        ZSTD_c_nbWorkers = 400

        def compress_ctypes(data: bytes, level: int = 19, ldm: bool = True) -> bytes:
            cctx = lib.ZSTD_createCCtx()
            if not cctx:
                raise RuntimeError("Failed to create ZSTD CCtx")
            try:
                lib.ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level)
                if ldm:
                    lib.ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1)
                # Enable multi-threaded compression if available
                workers = min(os.cpu_count() or 4, 8)
                lib.ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, workers)

                src_size = len(data)
                bound = lib.ZSTD_compressBound(src_size)
                out_buf = ctypes.create_string_buffer(bound)

                res = lib.ZSTD_compress2(cctx, out_buf, bound, data, src_size)
                if lib.ZSTD_isError(res):
                    err_msg = lib.ZSTD_getErrorName(res).decode('utf-8', errors='replace')
                    raise RuntimeError(f"ZSTD compression error: {err_msg}")
                return bytes(out_buf[:res])
            finally:
                lib.ZSTD_freeCCtx(cctx)

        return compress_ctypes
    except Exception as e:
        pass

    # 3. Fallback to CLI zstd if installed
    import subprocess
    import shutil
    zstd_bin = shutil.which("zstd")
    if zstd_bin:
        def compress_cli(data: bytes, level: int = 19, ldm: bool = True) -> bytes:
            cmd = [zstd_bin, f"-{level}", "-T0"]
            if ldm:
                cmd.append("--long")
            p = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            return p.stdout
        return compress_cli

    raise RuntimeError("No zstd compression implementation found (neither python-zstandard, libzstd, nor zstd CLI)")


def extract_build_stamp(data: bytes) -> str:
    m = re.search(rb'KARTPAD-BUILD [0-9]{4}-[0-9TZ:-]+', data)
    if m:
        return m.group(0).decode('ascii')
    return "KARTPAD-BUILD unknown"


def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

    parser = argparse.ArgumentParser(description="Pack game payloads for MKW Launcher romfs")
    parser.add_argument("--vanilla", default=os.path.join(repo_root, "build", "switch-fullapp", "kartpad.nro"),
                        help="Path to vanilla kartpad.nro")
    parser.add_argument("--rr", default=os.path.join(repo_root, "build", "switch-fullapp-rr", "kartpad-retro-rewind.nro"),
                        help="Path to retro rewind kartpad-retro-rewind.nro")
    parser.add_argument("--out-dir", default=os.path.join(repo_root, "build", "launcher", "romfs", "payload"),
                        help="Output directory for compressed payloads and payloads.json")
    args = parser.parse_args()

    compressor = get_zstd_compressor()

    targets = [
        ("vanilla", args.vanilla, "kartpad.nro.zst"),
        ("rr", args.rr, "kartpad-retro-rewind.nro.zst"),
    ]

    # Validate inputs exist
    for variant, nro_path, _ in targets:
        if not os.path.isfile(nro_path):
            sys.stderr.write(f"ERROR: Missing required game payload ({variant}): {nro_path}\n")
            sys.stderr.write("Build the game NRO first before packing payloads in payload mode.\n")
            sys.exit(1)

    os.makedirs(args.out_dir, exist_ok=True)
    manifest_entries = []

    print("==> Packing MKW Launcher game payloads (zstd level 19, long mode)...")

    for variant, nro_path, zst_filename in targets:
        t0 = time.time()
        with open(nro_path, "rb") as f:
            raw_data = f.read()

        raw_size = len(raw_data)
        raw_sha256 = hashlib.sha256(raw_data).hexdigest()
        build_stamp = extract_build_stamp(raw_data)

        print(f"Compressing {variant} ({nro_path}) [{raw_size:,} bytes, {build_stamp}]...")
        compressed_data = compressor(raw_data, level=19, ldm=True)
        zst_size = len(compressed_data)

        zst_path = os.path.join(args.out_dir, zst_filename)
        with open(zst_path, "wb") as f:
            f.write(compressed_data)

        elapsed = time.time() - t0
        ratio = raw_size / zst_size if zst_size > 0 else 0
        savings = (1.0 - zst_size / raw_size) * 100.0 if raw_size > 0 else 0
        print(f"  -> {zst_path}: {zst_size:,} bytes ({ratio:.2f}x ratio, {savings:.1f}% savings) in {elapsed:.2f}s")

        manifest_entries.append({
            "variant": variant,
            "file": zst_filename,
            "raw_size": raw_size,
            "raw_sha256": raw_sha256,
            "zst_size": zst_size,
            "build_stamp": build_stamp,
        })

    manifest = {
        "schema": 1,
        "payloads": manifest_entries,
    }

    manifest_path = os.path.join(args.out_dir, "payloads.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(f"==> Wrote payload manifest to: {manifest_path}")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
