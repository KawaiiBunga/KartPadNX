#!/usr/bin/env python3
"""Validate built game images and prove which graphics implementation linked."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

repo=Path(__file__).resolve().parents[1]
bin_dir=Path(os.environ.get('DEVKITPRO','/opt/devkitpro'))/'devkitA64/bin'
required={'dkDeviceCreate','dkQueueCreate','dkQueuePresentImage','dkQueueFlush','dkCmdBufDrawIndexed',
          '__wrap_wgpuQueueSubmit','__wrap_wgpuDeviceCreateRenderPipeline',
          '__wrap_wgpuCommandEncoderCopyTextureToBuffer',
          '__wrap_wgpuBindGroupLayoutAddRef','__wrap_wgpuBindGroupLayoutRelease',
          '__wrap_wgpuDeviceCreateShaderModule','__wrap_wgpuShaderModuleRelease',
          '__wrap_wgpuTextureCreateView','__wrap_wgpuTextureViewRelease'}
forbidden=re.compile(r'^(vkCreateInstance|vkGetInstanceProcAddr|vkCreateDevice|nvk_\w+|nouveau_ws_\w+)$')
results=[]

def romfs_file(nro, wanted):
    asset=struct.unpack_from('<I',nro,0x18)[0]
    assert nro[asset:asset+4]==b'ASET'
    offset,size=struct.unpack_from('<QQ',nro,asset+0x28)
    romfs=memoryview(nro)[asset+offset:asset+offset+size]
    header=struct.unpack_from('<10Q',romfs,0)
    assert header[0]==0x50
    dir_base,file_base,data_base=header[3],header[7],header[9]
    files={}
    def walk(offset,prefix):
        parent,sibling,child_dir,child_file,hash_next,name_len=struct.unpack_from('<6I',romfs,dir_base+offset)
        while child_file!=0xffffffff:
            parent,next_file,data_offset,length,hash_next,n=struct.unpack_from('<IIQQII',romfs,file_base+child_file)
            name=bytes(romfs[file_base+child_file+32:file_base+child_file+32+n]).decode()
            files[prefix+name]=romfs[data_base+data_offset:data_base+data_offset+length]
            child_file=next_file
        while child_dir!=0xffffffff:
            meta=struct.unpack_from('<6I',romfs,dir_base+child_dir)
            name=bytes(romfs[dir_base+child_dir+24:dir_base+child_dir+24+meta[5]]).decode()
            walk(child_dir,prefix+name+'/');child_dir=meta[1]
    walk(0,'')
    return bytes(files[wanted])

for directory,name in [('switch-fullapp','kartpad.nro'),('switch-fullapp-rr','kartpad-retro-rewind.nro')]:
    folder=repo/'build'/directory
    nro=folder/name
    data=nro.read_bytes()
    assert data[0x10:0x14]==b'NRO0', f'{name}: invalid NRO header'
    for cache_name in [b'pipeline_cache_deko.db',b'dawn_null_deko_cache.db']:
        assert cache_name in data,f'{name}: native cache namespace missing: {cache_name!r}'
    image_size=struct.unpack_from('<I',data,0x18)[0]
    assert 0x80<=image_size<=len(data), f'{name}: invalid NRO image size'
    elf=folder/'kartpad_full.elf'
    symbols=subprocess.check_output([str(bin_dir/'aarch64-none-elf-nm'),'-n',str(elf)],text=True)
    defined={parts[-1]:int(parts[0],16) for line in symbols.splitlines()
             if len(parts:=line.split())>=3 and re.fullmatch('[0-9a-fA-F]+',parts[0])}
    assert defined.get('_start')==0, f'{name}: entrypoint is not at zero'
    assert not (missing:=required-defined.keys()),f'{name}: missing native graphics symbols: {missing}'
    assert any(s.startswith('_Z20kartpad_deko_present') for s in defined),f'{name}: missing native presentation bridge'
    assert not (bad:=[s for s in defined if forbidden.match(s)]),f'{name}: linked legacy driver symbols: {bad[:10]}'
    mapping=(folder/'kartpad_full.map').read_text(errors='replace')
    assert not re.search(r'LOAD .*?(?:libnvk\.a|libvulkan\.a|libEGL\.a|libglapi\.a)',mapping),f'{name}: legacy graphics library in link map'
    header=subprocess.check_output([str(bin_dir/'aarch64-none-elf-readelf'),'-h',str(elf)],text=True)
    assert re.search(r'Machine:\s+AArch64',header),f'{name}: wrong CPU architecture'
    stamps=re.findall(rb'KARTPAD-BUILD [0-9]{4}-[0-9TZ:-]+',data)
    elf_stamps=set(re.findall(rb'KARTPAD-BUILD [0-9]{4}-[0-9TZ:-]+',elf.read_bytes()))
    assert stamps and set(stamps)==elf_stamps,f'{name}: NRO/ELF build stamps do not match'
    notes=subprocess.check_output([str(bin_dir/'aarch64-none-elf-readelf'),'-n',str(elf)],text=True)
    build_id=re.search(r'Build ID:\s+([0-9a-f]+)',notes).group(1)
    assert data[0x40:0x40+len(build_id)//2]==bytes.fromhex(build_id),f'{name}: NRO/ELF module IDs differ'
    item={'path':str(nro.relative_to(repo)),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
          'module_id':data[0x40:0x60].hex(),'elf_build_id':build_id,
          'build_stamp':stamps[0].decode() if stamps else None,'entrypoint':0,
          'native_graphics_symbols':sorted(required),'legacy_driver_symbols':[],
          'deko_debug_checks':'libdeko3dd.a' in mapping,
          'pixel_capture_available':b'deko-capture.flag' in data,
          'gx_isolation_probe_available':b'GX probe verdict' in data and b'probe opaque FS' in data,
          'scene_register_boundary':b'[scene-abi] repaired target=' in data,
          'archive_wait_checkpoint':b'[archive-checkpoint] return=' in data or b'[archive-abi] target=' in data,
          'archive_call_boundary':b'[archive-abi] target=' in data,
          'shader_training':b'shader training: session=' in data,
          'command_arena':any('CommandArena' in s for s in defined),
          'binding_cache':b'binding_cache.h' in elf.read_bytes(),
          'translucent_state_reuse':b'splitAlphaDirty' in elf.read_bytes(),
          'batched_dvd_index':b'index timing: %s %.1f ms since scan start' in data,
          'image_heap_reclamation':b'trim_image_chunks' in elf.read_bytes(),
          'menu_model_return_boundary':b'[menu-model-abi] target=' in data,
          'globe_scene_boundary':b'[globe-abi] target=' in data,
          'gx_packed_uniform_header':b'vec4u, 3' in data and b'array_start: array<{11}>' in data,
          'hardware_tested':False}
    if item['deko_debug_checks']:
        assert item['gx_isolation_probe_available'],f'{name}: checked diagnostic lacks GX isolation probe'
    else:
        assert 'libdeko3d.a' in mapping,f'{name}: release deko3d library missing'
        assert not item['gx_isolation_probe_available'],f'{name}: GX isolation probe leaked into release'
        assert not item['pixel_capture_available'],f'{name}: SD flag can activate captures in release'
    assert item['gx_packed_uniform_header'],f'{name}: missing packed native GX uniform header'
    assert item['scene_register_boundary'],f'{name}: missing scene register boundary repair'
    assert item['archive_wait_checkpoint'],f'{name}: missing blocking archive register checkpoint'
    assert item['archive_call_boundary'],f'{name}: missing returning archive boundary'
    assert item['shader_training'],f'{name}: missing persistent shader session collection'
    assert item['command_arena'],f'{name}: missing native command arena'
    assert item['binding_cache'],f'{name}: missing native binding reuse'
    assert item['translucent_state_reuse'],f'{name}: missing translucent state reuse'
    assert item['batched_dvd_index'],f'{name}: missing native directory index integration'
    assert item['image_heap_reclamation'],f'{name}: missing fenced image heap reclamation'
    assert item['globe_scene_boundary'],f'{name}: missing GlobeScene returning-call boundary'
    if directory.endswith('-rr'):
        assert item['menu_model_return_boundary'],f'{name}: missing RR menu-model repair'
    seed=repo/'private/deko-training/initial_dksh_cache.bin'
    embedded=romfs_file(data,'deko/initial_dksh_cache.bin')
    assert embedded==seed.read_bytes(),f'{name}: embedded native shader pack differs from validated input'
    item['embedded_shader_pack_sha256']=hashlib.sha256(embedded).hexdigest()
    item['embedded_shader_pack_bytes']=len(embedded)
    results.append(item)
    print(f'{name}: valid AArch64 NRO, _start=0, native deko3d linked, Mesa/NVK driver symbols absent')
out=repo/'private/deko-migration/artifacts.json'
out.write_text(json.dumps({'switchwaker_revision':'04372277d03f934e1e563ae302bba9f565c5cb5d','artifacts':results},indent=2)+'\n')
print(f'Wrote {out}')
