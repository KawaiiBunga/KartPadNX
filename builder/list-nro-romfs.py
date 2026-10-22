#!/usr/bin/env python3
"""List the files in an NRO's embedded RomFS, or prove that it carries no game.

  list-nro-romfs.py NRO                 # print size and path of every RomFS file
  list-nro-romfs.py NRO --game-free     # exit 1 if any file is a game payload

A game payload is anything under payload/, any *.nro or *.nro.zst file, or any
file whose bytes are an NRO image. The check reads the packaged NRO itself, so
it does not depend on how the build staged its RomFS.
"""
import argparse
import struct
import sys

NO_ENTRY = 0xFFFFFFFF


def romfs_files(nro):
    """Returns {path: bytes} for every file in the NRO's RomFS (empty if it has none)."""
    if nro[0x10:0x14] != b'NRO0':
        raise ValueError('not an NRO image')
    nro_size = struct.unpack_from('<I', nro, 0x18)[0]
    if nro[nro_size:nro_size + 4] != b'ASET':
        return {}
    romfs_offset, romfs_size = struct.unpack_from('<QQ', nro, nro_size + 0x28)
    romfs = memoryview(nro)[nro_size + romfs_offset:nro_size + romfs_offset + romfs_size]
    if not romfs_size:
        return {}

    # RomFS header: header size, then (offset, size) pairs for the directory hash,
    # directory metadata, file hash and file metadata tables, then the data offset.
    header = struct.unpack_from('<10Q', romfs, 0)
    dir_table, file_table, data_base = header[3], header[7], header[9]
    files = {}

    def walk(dir_offset, prefix):
        _, _, child_dir, child_file, _, _ = struct.unpack_from('<6I', romfs, dir_table + dir_offset)
        while child_file != NO_ENTRY:
            _, next_file, data_offset, size, _, name_len = struct.unpack_from('<IIQQII', romfs, file_table + child_file)
            name = bytes(romfs[file_table + child_file + 0x20:file_table + child_file + 0x20 + name_len]).decode()
            files[prefix + name] = bytes(romfs[data_base + data_offset:data_base + data_offset + size])
            child_file = next_file
        while child_dir != NO_ENTRY:
            _, next_dir, _, _, _, name_len = struct.unpack_from('<6I', romfs, dir_table + child_dir)
            name = bytes(romfs[dir_table + child_dir + 0x18:dir_table + child_dir + 0x18 + name_len]).decode()
            walk(child_dir, prefix + name + '/')
            child_dir = next_dir

    walk(0, '')
    return files


def is_game_payload(path, data):
    return (path.startswith('payload/') or path.endswith(('.nro', '.nro.zst'))
            or data[0x10:0x14] == b'NRO0')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('nro', help='NRO file to inspect')
    parser.add_argument('--game-free', action='store_true', help='Fail if the RomFS contains a game payload')
    args = parser.parse_args()

    with open(args.nro, 'rb') as f:
        files = romfs_files(f.read())

    if not args.game_free:
        for path, data in sorted(files.items()):
            print(f'{len(data):12,}  {path}')
        return 0

    payloads = sorted(path for path, data in files.items() if is_game_payload(path, data))
    if payloads:
        print(f'ERROR: {args.nro} contains game payloads:', file=sys.stderr)
        for path in payloads:
            print(f'  {path}', file=sys.stderr)
        return 1
    print(f'OK: {args.nro} RomFS has {len(files)} files and no game payload')
    return 0


if __name__ == '__main__':
    sys.exit(main())
