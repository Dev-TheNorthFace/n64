# mipsasm.py : mini-assembleur MIPS (big-endian) pour la ROM de demo maison.
# Pseudo-instructions : li, la, move, b, nop.  Etiquettes "nom:", constantes "NOM = valeur".
import re, struct, sys

REGS = {n: i for i, n in enumerate(
    "zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}

def reg(t):
    t = t.strip().lstrip('$')
    if t in REGS: return REGS[t]
    if t.isdigit(): return int(t)
    raise SystemExit('registre inconnu : ' + t)

def num(t, syms):
    t = t.strip()
    m = re.match(r'^(.+?)\s*([+-])\s*(\w+)$', t)
    if m and not t.startswith('-'):
        a = num(m.group(1), syms); b = num(m.group(3), syms)
        return a + b if m.group(2) == '+' else a - b
    if t.startswith('%hi(') and t.endswith(')'):
        v = num(t[4:-1], syms); return ((v >> 16) + ((v >> 15) & 1)) & 0xFFFF
    if t.startswith('%lo(') and t.endswith(')'):
        return num(t[4:-1], syms) & 0xFFFF
    if t.startswith('-'): return -num(t[1:], syms)
    if t.lower().startswith('0x'): return int(t, 16)
    if t[0].isdigit(): return int(t)
    if t in syms: return syms[t]
    raise KeyError(t)

R3 = {'addu': 0x21, 'subu': 0x23, 'and': 0x24, 'or': 0x25, 'xor': 0x26, 'nor': 0x27, 'slt': 0x2A, 'sltu': 0x2B}
SH = {'sll': 0, 'srl': 2, 'sra': 3}
IM = {'addiu': 9, 'slti': 10, 'sltiu': 11, 'andi': 12, 'ori': 13, 'xori': 14}
MEM = {'lb': 32, 'lh': 33, 'lw': 35, 'lbu': 36, 'lhu': 37, 'sb': 40, 'sh': 41, 'sw': 43}
BR2 = {'beq': 4, 'bne': 5}
BR1 = {'blez': (6, 0), 'bgtz': (7, 0), 'bltz': (1, 0), 'bgez': (1, 1)}

def expand(mn, ops):
    if mn == 'nop': return [('sll', ['zero', 'zero', '0'])]
    if mn == 'move': return [('addu', [ops[0], ops[1], 'zero'])]
    if mn == 'b': return [('beq', ['zero', 'zero', ops[0]])]
    if mn in ('li', 'la'): return [('lui', [ops[0], '%hi(' + ops[1] + ')']), ('addiu', [ops[0], ops[0], '%lo(' + ops[1] + ')'])]
    return [(mn, ops)]

def assemble(text, base):
    lines = []
    for raw in text.splitlines():
        line = raw.split('#')[0].split(';')[0].strip()
        if line: lines.append(line)
    syms = {}
    for final in (False, True):
        pc = base; out = bytearray()
        for line in lines:
            m = re.match(r'^(\w+)\s*=\s*(.+)$', line)
            if m: syms[m.group(1)] = num(m.group(2), syms); continue
            m = re.match(r'^(\w+):\s*(.*)$', line)
            if m:
                syms[m.group(1)] = pc; line = m.group(2)
                if not line: continue
            parts = line.split(None, 1)
            mn = parts[0].lower()
            ops = [o.strip() for o in parts[1].split(',')] if len(parts) > 1 else []
            if mn == '.word':
                for o in ops:
                    try: v = num(o, syms)
                    except KeyError:
                        if final: raise
                        v = 0
                    out += struct.pack('>I', v & 0xFFFFFFFF); pc += 4
                continue
            if mn == '.half':
                for o in ops: out += struct.pack('>H', num(o, syms) & 0xFFFF); pc += 2
                continue
            if mn == '.align':
                al = int(ops[0]) if ops else 4
                while pc % al: out += b'\0'; pc += 1
                continue
            for mn2, ops2 in expand(mn, ops):
                def N(t):
                    try: return num(t, syms)
                    except KeyError:
                        if final: raise SystemExit('symbole inconnu : ' + t + ' dans : ' + line)
                        return 0
                if mn2 in R3: w = reg(ops2[1]) << 21 | reg(ops2[2]) << 16 | reg(ops2[0]) << 11 | R3[mn2]
                elif mn2 in SH: w = reg(ops2[1]) << 16 | reg(ops2[0]) << 11 | (N(ops2[2]) & 31) << 6 | SH[mn2]
                elif mn2 in IM: w = IM[mn2] << 26 | reg(ops2[1]) << 21 | reg(ops2[0]) << 16 | (N(ops2[2]) & 0xFFFF)
                elif mn2 == 'lui': w = 15 << 26 | reg(ops2[0]) << 16 | (N(ops2[1]) & 0xFFFF)
                elif mn2 in MEM:
                    mm = re.match(r'^(.*)\((.+)\)$', ops2[1]); off = N(mm.group(1) or '0')
                    w = MEM[mn2] << 26 | reg(mm.group(2)) << 21 | reg(ops2[0]) << 16 | (off & 0xFFFF)
                elif mn2 in BR2:
                    off = (N(ops2[2]) - (pc + 4)) >> 2
                    w = BR2[mn2] << 26 | reg(ops2[0]) << 21 | reg(ops2[1]) << 16 | (off & 0xFFFF)
                elif mn2 in BR1:
                    op, rt = BR1[mn2]; off = (N(ops2[1]) - (pc + 4)) >> 2
                    w = op << 26 | reg(ops2[0]) << 21 | rt << 16 | (off & 0xFFFF)
                elif mn2 in ('j', 'jal'): w = (2 if mn2 == 'j' else 3) << 26 | ((N(ops2[0]) >> 2) & 0x3FFFFFF)
                elif mn2 == 'jr': w = reg(ops2[0]) << 21 | 8
                elif mn2 == 'mult': w = reg(ops2[0]) << 21 | reg(ops2[1]) << 16 | 0x18
                elif mn2 == 'mflo': w = reg(ops2[0]) << 11 | 0x12
                else: raise SystemExit('instruction inconnue : ' + mn2)
                out += struct.pack('>I', w & 0xFFFFFFFF); pc += 4
    return bytes(out), syms

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    code, syms = assemble(open(src).read(), 0x80000400)
    rom = bytearray(0x101000)
    hdr = struct.pack('>IIIIII', 0x80371240, 0x0000000F, 0x80000400, 0x0000144C, 0, 0)
    rom[0:len(hdr)] = hdr
    rom[0x20:0x34] = b'DEMO N64 MAISON     '
    rom[0x3B:0x40] = b'NDMEF'[0:4] + b'\x00'
    rom[0x3B] = ord('N'); rom[0x3C] = ord('D'); rom[0x3D] = ord('M'); rom[0x3E] = ord('E'); rom[0x3F] = 0
    rom[0x1000:0x1000 + len(code)] = code
    open(dst, 'wb').write(bytes(rom))
    print('%s : %d instructions, %d symboles' % (dst, len(code) // 4, len(syms)))
