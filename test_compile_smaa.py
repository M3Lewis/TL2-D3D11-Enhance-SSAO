import ctypes
import os

d3dcompiler = ctypes.windll.LoadLibrary('d3dcompiler_47.dll')

with open(r'tl2_d3d11_route_b/src/d3d11_smaa_shaders.h', 'r', encoding='utf-8') as f:
    text = f.read()

import re

marker = 'kSmaaShadersSource[] = R"HLSL(\n'
start = text.find(marker) + len(marker)
end = text.find(')HLSL";', start)
raw_part = text[start:end]
clean_hlsl = re.sub(r'\)HLSL"[\s\r\n]*R"HLSL\(', '', raw_part)
shader_src = clean_hlsl.encode('utf-8')
print("Extracted shader length:", len(shader_src))

class ID3D10BlobVtbl(ctypes.Structure):
    pass

class ID3D10Blob(ctypes.Structure):
    _fields_ = [('lpVtbl', ctypes.POINTER(ID3D10BlobVtbl))]

ID3D10BlobVtbl._fields_ = [
    ('QueryInterface', ctypes.c_void_p),
    ('AddRef', ctypes.c_void_p),
    ('Release', ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)),
    ('GetBufferPointer', ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)),
    ('GetBufferSize', ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)),
]

d3dcompiler.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.c_void_p]
d3dcompiler.D3DDisassemble.restype = ctypes.c_long

for entry in ['PS_SmaaEdgeDetection', 'PS_SmaaBlendingWeight', 'PS_SmaaNeighborhoodBlend']:
    blob = ctypes.POINTER(ID3D10Blob)()
    err = ctypes.POINTER(ID3D10Blob)()
    hr = d3dcompiler.D3DCompile(
        shader_src, len(shader_src), None, None, None,
        entry.encode('utf-8'), b'ps_4_0', 0, 0,
        ctypes.byref(blob), ctypes.byref(err)
    )
    err_msg = ''
    if err:
        ptr = err.contents.lpVtbl.contents.GetBufferPointer(err)
        size = err.contents.lpVtbl.contents.GetBufferSize(err)
        err_msg = ctypes.string_at(ptr, size).decode('utf-8', errors='ignore')
        err.contents.lpVtbl.contents.Release(err)
    print(f'{entry}: hr={hr:#x}')
    if blob and entry == 'PS_SmaaBlendingWeight':
        disasm = ctypes.POINTER(ID3D10Blob)()
        ptr = blob.contents.lpVtbl.contents.GetBufferPointer(blob)
        size = blob.contents.lpVtbl.contents.GetBufferSize(blob)
        d3dcompiler.D3DDisassemble(ptr, size, 0, None, ctypes.byref(disasm))
        if disasm:
            dptr = disasm.contents.lpVtbl.contents.GetBufferPointer(disasm)
            dsize = disasm.contents.lpVtbl.contents.GetBufferSize(disasm)
            asm = ctypes.string_at(dptr, dsize).decode('utf-8', errors='ignore')
            for idx in range(125, 175):
                print(f'{idx}: {asm.splitlines()[idx]}')
            disasm.contents.lpVtbl.contents.Release(disasm)
        blob.contents.lpVtbl.contents.Release(blob)
    if err_msg:
        print(f'  err={err_msg}')
