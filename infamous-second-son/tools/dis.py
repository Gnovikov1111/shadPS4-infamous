"""Find byte patterns in the unwrapped eboot and disassemble around them.
usage: dis.py <elf> <before> <after> <name=pattern> ...   (pattern: hex bytes, ?? wildcard)
       dis.py <elf> <before> <after> @0xVA ...             (disassemble at a virtual address)"""
import re, shutil, subprocess, sys, os
elf, before, after = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
data = open(elf, 'rb').read()
TEXT_OFF, TEXT_SIZE = 0x4000, 0xcc55b8          # LOAD R E : file 0x4000 -> VA 0
work = os.path.join(os.path.dirname(elf), 'text.bin')
if not os.path.exists(work):
    open(work, 'wb').write(data[TEXT_OFF:TEXT_OFF + TEXT_SIZE])
text = open(work, 'rb').read()
OBJDUMP = os.environ.get('OBJDUMP') or shutil.which('objdump') or 'objdump'  # GNU binutils objdump
def dis(va, mark):
    out = subprocess.run([OBJDUMP, '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '--no-show-raw-insn',
                          f'--start-address={max(0, va - before):#x}', f'--stop-address={va + after:#x}', work],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s*(.*)', line)
        if m:
            a = int(m.group(1), 16)
            print(('>> ' if a <= mark < a + 1 or a == mark else '   ') + f'{a:8x}: {m.group(2)}')
for arg in sys.argv[4:]:
    if arg.startswith('@'):
        va = int(arg[1:], 16); print(f'===== @{va:#x}'); dis(va, va); continue
    name, pat = arg.split('=', 1)
    rx = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in pat.split())
    hits = [m.start() for m in re.finditer(rx, text, re.S)]
    print(f'===== {name}: {len(hits)} hit(s) {[hex(h) for h in hits[:8]]}')
    for h in hits[:3]:
        dis(h, h)
