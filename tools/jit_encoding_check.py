#!/usr/bin/env python3
# Compare l'encodeur PowerPC du recompilateur (source/jit.c) avec l'assembleur LLVM.
import os, re, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = """
addi 4, 31, -8
addis 5, 0, 0x1234
ori 4, 5, 0xBEEF
oris 6, 7, 0x8000
xori 8, 9, 0xFFFF
andi. 4, 4, 31
lwz 4, 320(31)
stw 4, 12(31)
ld 7, 248(31)
std 7, 16(31)
stdu 1, -160(1)
mr 31, 3
extsw 4, 4
extsb 7, 7
add 4, 4, 5
subf 4, 5, 4
and 4, 4, 5
or 4, 4, 5
xor 4, 4, 5
nor 4, 4, 5
slw 4, 4, 5
srw 4, 4, 5
sraw 4, 4, 5
sld 4, 4, 5
srd 4, 4, 5
srad 4, 4, 5
lbzx 7, 30, 6
lhax 7, 30, 6
lhzx 7, 30, 6
lwzx 7, 30, 6
ldx 7, 30, 6
stbx 7, 30, 6
sthx 7, 30, 6
stwx 7, 30, 6
stdx 7, 30, 6
rlwinm 5, 4, 2, 30, 31
rlwinm 6, 4, 0, 3, 31
rlwinm 8, 6, 20, 12, 31
rlwinm 4, 4, 9, 31, 31
rlwinm 4, 4, 5, 0, 26
rlwinm 4, 4, 27, 5, 31
rldicr 4, 4, 5, 58
rldicr 4, 4, 32, 31
rldicr 4, 4, 40, 23
rldicl 4, 4, 59, 5
rldicl 4, 4, 20, 44
sradi 4, 4, 3
sradi 4, 4, 35
srawi 4, 4, 7
cmpd 4, 5
cmpld 4, 5
cmplw 6, 28
cmpdi 4, -3
cmpwi 5, 2
mtctr 12
bctrl
beq 0, .+12
bge 0, .+8
nop
b .+8
nop
lis 12, 0x0123
ori 12, 12, 0x4567
rldicr 12, 12, 32, 31
oris 12, 12, 0x89AB
ori 12, 12, 0xCDEF
andis. 10, 9, 0x0400
rldimi 8, 7, 0, 32
rldimi 8, 7, 16, 8
mulld 6, 4, 5
rldicl 4, 4, 0, 32
bctr
"""
tmp = tempfile.mkdtemp()
src = os.path.join(tmp, "a.s"); obj = os.path.join(tmp, "a.o")
open(src, "w").write("\t" + "\n\t".join(l for l in REF.strip().splitlines()) + "\n")
import shutil
ps3 = os.environ.get("PS3DEV", "/usr/local/ps3dev")
gas = [os.path.join(ps3, "ppu", "bin", "powerpc64-ps3-elf-" + t) for t in ("as", "objdump")]
if shutil.which("clang") and shutil.which("llvm-objdump"):
    subprocess.check_call(["clang", "--target=powerpc64-unknown-linux-gnu", "-c", src, "-o", obj])
    dis = subprocess.check_output(["llvm-objdump", "-d", obj]).decode()
elif os.path.exists(gas[0]) and os.path.exists(gas[1]):          # chaine PS3 (GitHub)
    subprocess.check_call([gas[0], "-a64", "-o", obj, src])
    dis = subprocess.check_output([gas[1], "-d", obj]).decode()
else:
    print("Encodage PowerPC : aucun assembleur PowerPC trouve, verification sautee")
    sys.exit(0)
ref = [int("".join(m.group(1).split()), 16) for m in re.finditer(r"^\s+[0-9a-f]+:\s+((?:[0-9a-f]{2} ){4})", dis, re.M)]
prog = os.path.join(tmp, "enc")
c = os.path.join(tmp, "m.c")
open(c, "w").write('#include <stdio.h>\n#include <stdint.h>\nint jit_encoder_selftest(uint32_t*,int);\n'
                   'int main(void){uint32_t w[256];int n=jit_encoder_selftest(w,256),i;for(i=0;i<n;i++)printf("%08X\\n",w[i]);return 0;}\n')
subprocess.check_call(["gcc", "-std=gnu99", "-DJIT_SELFTEST", "-I" + os.path.join(ROOT, "source"), c,
                       os.path.join(ROOT, "source", "jit.c"), os.path.join(ROOT, "source", "vr4300.c"), "-o", prog, "-lm"])
mine = [int(x, 16) for x in subprocess.check_output([prog]).decode().split()]
lines = REF.strip().splitlines()
bad = 0
for i, (a, b) in enumerate(zip(mine, ref)):
    if a != b:
        bad += 1
        print("DIFFERENT : %-28s jit %08X  llvm %08X" % (lines[i] if i < len(lines) else "?", a, b))
if len(mine) != len(ref):
    bad += 1
    print("nombre d'instructions different : jit %d, llvm %d" % (len(mine), len(ref)))
print("Encodage PowerPC : %d instructions comparees a l'assembleur de reference, %d differences" % (len(ref), bad))
sys.exit(1 if bad else 0)
