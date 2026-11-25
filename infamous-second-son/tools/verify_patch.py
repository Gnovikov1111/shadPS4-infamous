"""Simulate shadPS4 'mask' patch application on the unwrapped eboot, in file order.
usage: verify_patch.py <elf> <patch.xml> [disasm_va ...]"""
import re, shutil, subprocess, sys, os, xml.etree.ElementTree as ET
elf_path, xml_path = sys.argv[1], sys.argv[2]
orig = open(elf_path, 'rb').read()
def rx(pat):
    return re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in pat.split()), re.S)
ok_all = True
for md in ET.parse(xml_path).getroot().iter('Metadata'):
    img = bytearray(orig)
    print(f"== {md.get('Name')}  (AppVer {md.get('AppVer')}, enabled={md.get('isEnabled')})")
    for ln in md.find('PatchList'):
        assert ln.get('Type') == 'mask', 'only mask lines are simulated'
        r = rx(ln.get('Address')); hits = [m.start() for m in r.finditer(bytes(img))]
        val = bytes.fromhex(ln.get('Value')); off = int(ln.get('Offset'))
        status = 'OK' if len(hits) == 1 else ('NO MATCH' if not hits else f'{len(hits)} MATCHES (first used)')
        if len(hits) != 1: ok_all = False
        if hits:
            at = hits[0] + off; before = img[at:at + len(val)].hex()
            img[at:at + len(val)] = val
            print(f"   {status:22} VA {at - 0x4000:#09x}  {before} -> {val.hex()}   [{ln.get('Address')}] +{off}")
        else:
            print(f"   {status:22} [{ln.get('Address')}]")
    if len(sys.argv) > 3:
        tmp = os.path.join(os.path.dirname(elf_path), 'text_patched.bin')
        open(tmp, 'wb').write(bytes(img[0x4000:0x4000 + 0xcc55b8]))
        for a in sys.argv[3:]:
            s, e = (int(x, 16) for x in a.split('-'))
            out = subprocess.run([os.environ.get('OBJDUMP') or shutil.which('objdump') or 'objdump','-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel',
                                  '--no-show-raw-insn', f'--start-address={s:#x}', f'--stop-address={e:#x}', tmp], capture_output=True, text=True).stdout
            print('\n'.join('      ' + l.strip() for l in out.splitlines() if re.match(r'\s*[0-9a-f]+:', l)))
print('ALL PATTERNS UNIQUE' if ok_all else 'SOME PATTERNS NOT UNIQUE')
