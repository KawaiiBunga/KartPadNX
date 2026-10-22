"""Builds a fake SD card + romfs for the launcher preview in the current directory.

Creates "romfs:/" (a copy of launcher/romfs plus a payloads.json) and "sdmc:/" with verified
game data, a Retro Rewind pack, a vanilla save with two licenses and a Mii database.
Nothing here is game data: every file is a placeholder with the shape the launcher checks.
"""
import json
import os
import shutil
import struct
import sys
import zlib

repo_launcher = sys.argv[1]
here = os.getcwd()

shutil.rmtree("romfs:", ignore_errors=True)
shutil.rmtree("sdmc:", ignore_errors=True)
shutil.copytree(os.path.join(repo_launcher, "romfs"), "romfs:")
os.makedirs("romfs:/payload", exist_ok=True)
json.dump({"schema": 1, "payloads": [
    {"variant": "vanilla", "file": "kartpad.nro.zst", "raw_size": 1, "raw_sha256": "0" * 64, "zst_size": 1,
     "build_stamp": "KARTPAD-BUILD 2026-10-05T09:08:04Z"},
    {"variant": "rr", "file": "kartpad-retro-rewind.nro.zst", "raw_size": 1, "raw_sha256": "0" * 64, "zst_size": 1,
     "build_stamp": "KARTPAD-BUILD 2026-10-05T09:18:18Z"},
]}, open("romfs:/payload/payloads.json", "w"))


def write(path, data, mode="wb"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f:
        f.write(data)


k = "sdmc:/switch/kartpad"
# Disc files: the host build hashes via "<file>.sha256" sidecars.
write(f"{k}/data/sys/main.dol", b"dol")
write(f"{k}/data/sys/main.dol.sha256", "80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05", "w")
write(f"{k}/data/files/rel/StaticR.rel", b"rel")
write(f"{k}/data/files/rel/StaticR.rel.sha256", "16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d", "w")
# Retro Rewind pack.
rr = f"{k}/retro-rewind/RetroRewind6"
write(f"{rr}/xml/RetroRewind6.xml", "<wiidisc/>", "w")
write(f"{rr}/Binaries/Code.pul", b"pul")
write(f"{rr}/version.txt", "6.12.8\n", "w")


def utf16_name(name):
    return name.encode("utf-16-be").ljust(20, b"\0")[:20]


# Vanilla save: licenses "Kawaii" and "Luigi".
save = bytearray(0x2BC000)
save[0:8] = b"RKSD0006"
for i, (name, vr, br) in enumerate([("Kawaii", 7432, 5120), ("Luigi", 5000, 5000)]):
    base = 0x08 + i * 0x8CC0
    save[base:base + 4] = b"RKPD"
    save[base + 0x14:base + 0x28] = utf16_name(name)
    save[base + 0xB0:base + 0xB4] = struct.pack(">HH", vr, br)
save[0x27FFC:0x28000] = struct.pack(">I", zlib.crc32(bytes(save[:0x27FFC])) & 0xFFFFFFFF)
write(f"{k}/NAND/title/00010004/524d4350/data/rksys.dat", bytes(save))

# Mii database with a few Miis.
db = bytearray(0x1F1E0)
db[0:4] = b"RNOD"
db[0x1D00:0x1D04] = b"RNHD"
for i, (name, female, color) in enumerate([("Kawaii", 1, 7), ("Bunga", 0, 5), ("Daisy", 1, 1), ("Toad", 0, 0),
                                           ("Peach", 1, 7), ("Wario", 0, 2), ("Yoshi", 0, 4)]):
    off = 4 + i * 0x4A
    flags = (female << 14) | (color << 1)
    db[off:off + 2] = struct.pack(">H", flags)
    db[off + 2:off + 22] = utf16_name(name)
    db[off + 0x16] = 64
    db[off + 0x17] = 64
    db[off + 0x18:off + 0x1C] = struct.pack(">I", 0x80000000 | (i + 1))


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


db[0x1F1DE:0x1F1E0] = struct.pack(">H", crc16(bytes(db[:0x1F1DE])))
write(f"{k}/NAND/shared2/menu/FaceLib/RFL_DB.dat", bytes(db))
os.makedirs("sdmc:/switch/mkw-launcher/saves/vanilla/20261005-143000", exist_ok=True)
write("sdmc:/switch/mkw-launcher/saves/vanilla/20261005-143000/rksys.dat", bytes(save))
print("fixture ready in", here)
