"""Resolve PS4 import NIDs in the unwrapped eboot to PLT stubs and their call sites.
usage: imports.py <elf> <functionName> ..."""
import base64, hashlib, re, struct, sys
KEY = bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')
def nid(name):
    h = hashlib.sha1(name.encode() + KEY).digest()
    return base64.b64encode(struct.pack('>Q', struct.unpack('<Q', h[:8])[0]), b'+-').rstrip(b'=').decode()
elf = open(sys.argv[1], 'rb').read()
phoff, phnum = struct.unpack_from('<Q', elf, 0x20)[0], struct.unpack_from('<H', elf, 0x38)[0]
ph = [struct.unpack_from('<IIQQQQQQ', elf, phoff + i * 56) for i in range(phnum)]
dyn = next(p for p in ph if p[0] == 2)                 # PT_DYNAMIC
dlib = next(p for p in ph if p[0] == 0x61000000)       # PT_SCE_DYNLIBDATA
tags = {}
for i in range(dyn[5] // 16):
    t, v = struct.unpack_from('<qQ', elf, dyn[2] + i * 16)
    tags.setdefault(t, v)
base = dlib[2]
strtab, symtab = base + tags[0x61000035], base + tags[0x61000039]
jmprel, jmpsz = base + tags[0x61000029], tags[0x6100002d]
def symname(i):
    st_name = struct.unpack_from('<I', elf, symtab + i * 24)[0]
    end = elf.index(b'\0', strtab + st_name)
    return elf[strtab + st_name:end].decode()
got = {}
for i in range(jmpsz // 24):
    off, info, add = struct.unpack_from('<QQq', elf, jmprel + i * 24)
    got[off] = symname(info >> 32)
print(f'{len(got)} PLT imports')
text = elf[0x4000:0x4000 + 0xcc55b8]                   # VA 0
stubs = {}
for m in re.finditer(rb'\xff\x25', text, re.S):
    va = m.start(); tgt = va + 6 + struct.unpack_from('<i', text, va + 2)[0]
    if tgt in got: stubs.setdefault(got[tgt], []).append(va)
calls = {}
for m in re.finditer(rb'\xe8', text, re.S):
    va = m.start()
    if va + 5 > len(text): continue
    calls.setdefault(va + 5 + struct.unpack_from('<i', text, va + 1)[0], []).append(va)
for name in sys.argv[2:]:
    n = nid(name)
    syms = [s for s in set(got.values()) if s.split('#')[0] == n]
    print(f'== {name}  NID {n}  imported: {bool(syms)}')
    for s in syms:
        for st in stubs.get(s, []):
            print(f'   PLT stub {st:#x}  calls from: {[hex(c) for c in calls.get(st, [])][:12]}')
