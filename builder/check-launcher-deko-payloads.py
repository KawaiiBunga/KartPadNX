#!/usr/bin/env python3
"""Read the built launcher's actual RomFS and verify both embedded native NROs."""
import ctypes
import ctypes.util
import hashlib
import json
from pathlib import Path
import struct

repo=Path(__file__).resolve().parents[1]
nro=(repo/'build/launcher/mkw-launcher.nro').read_bytes()
assert nro[0x10:0x14]==b'NRO0'
asset=struct.unpack_from('<I',nro,0x18)[0]
assert nro[asset:asset+4]==b'ASET','Missing NRO assets'
romfs_offset,romfs_size=struct.unpack_from('<QQ',nro,asset+0x28)
romfs=memoryview(nro)[asset+romfs_offset:asset+romfs_offset+romfs_size]
header=struct.unpack_from('<10Q',romfs,0)
assert header[0]==0x50,'Unexpected RomFS header'
dir_base,file_base,data_base=header[3],header[7],header[9]
files={}
def directory(offset,prefix):
    parent,sibling,child_dir,child_file,hash_next,name_len=struct.unpack_from('<6I',romfs,dir_base+offset)
    while child_file!=0xffffffff:
        parent,next_file,data_offset,size,hash_next,length=struct.unpack_from('<IIQQII',romfs,file_base+child_file)
        name=bytes(romfs[file_base+child_file+0x20:file_base+child_file+0x20+length]).decode()
        files[prefix+name]=romfs[data_base+data_offset:data_base+data_offset+size]
        child_file=next_file
    while child_dir!=0xffffffff:
        meta=struct.unpack_from('<6I',romfs,dir_base+child_dir)
        name=bytes(romfs[dir_base+child_dir+0x18:dir_base+child_dir+0x18+meta[5]]).decode()
        directory(child_dir,prefix+name+'/')
        child_dir=meta[1]
directory(0,'')
manifest=json.loads(bytes(files['payload/payloads.json']))
lib=ctypes.CDLL(ctypes.util.find_library('zstd'))
lib.ZSTD_decompress.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t]
lib.ZSTD_decompress.restype=ctypes.c_size_t
lib.ZSTD_isError.argtypes=[ctypes.c_size_t]
lib.ZSTD_isError.restype=ctypes.c_uint
expected={'vanilla':repo/'build/switch-fullapp/kartpad.nro',
          'rr':repo/'build/switch-fullapp-rr/kartpad-retro-rewind.nro'}
assert {p['variant'] for p in manifest['payloads']}==set(expected)
for item in manifest['payloads']:
    raw=expected[item['variant']].read_bytes()
    compressed=bytes(files['payload/'+item['file']])
    assert len(compressed)==item['zst_size']
    output=ctypes.create_string_buffer(item['raw_size'])
    size=lib.ZSTD_decompress(output,len(output),compressed,len(compressed))
    assert not lib.ZSTD_isError(size) and size==len(raw)==item['raw_size']
    unpacked=output.raw[:size]
    assert unpacked==raw,'Embedded payload differs from the built NRO'
    assert hashlib.sha256(unpacked).hexdigest()==item['raw_sha256']
    print(f"Launcher {item['variant']} payload: decompressed bytes and SHA-256 match native NRO")
receipt={'launcher_bytes':len(nro),'launcher_sha256':hashlib.sha256(nro).hexdigest(),
         'embedded_payloads':manifest['payloads'],'source':'actual NRO RomFS','hardware_tested':False}
(repo/'private/deko-migration/launcher-check.json').write_text(json.dumps(receipt,indent=2)+'\n')
