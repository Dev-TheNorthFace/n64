# make_demo.py : construit la ROM de demo maison (cube 3D) et source/demo_rom.c
#   python3 tools/make_demo.py
# Assemble tools/demo.s puis ajoute les donnees generees ici :
# 128 matrices de rotation, 24 sommets textures, texture damier 32x32,
# liste d'affichage F3DEX2, chaine d'identification du microcode.
import math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mipsasm import assemble

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def fixmtx(m):
    """matrice 4x4 (lignes) -> 16 mots de 16 bits entiers puis 16 fractions"""
    ints, fracs = [], []
    for i in range(4):
        for j in range(4):
            v = int(round(m[i][j] * 65536)) & 0xFFFFFFFF
            ints.append(v >> 16); fracs.append(v & 0xFFFF)
    return ints + fracs

def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]

def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0, 0], [0, c, s, 0], [0, -s, c, 0], [0, 0, 0, 1]]

def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, -s, 0], [0, 1, 0, 0], [s, 0, c, 0], [0, 0, 0, 1]]

def trans(x, y, z):
    return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [x, y, z, 1]]

def perspective(fovy, aspect, near, far):
    f = 1 / math.tan(math.radians(fovy) / 2)
    return [[f / aspect, 0, 0, 0], [0, f, 0, 0],
            [0, 0, (far + near) / (near - far), -1], [0, 0, 2 * far * near / (near - far), 0]]

def halves(vals):
    return ['        .half   ' + ', '.join('0x%04X' % (v & 0xFFFF) for v in vals[i:i + 8]) for i in range(0, len(vals), 8)]

def words(vals):
    return ['        .word   ' + ', '.join(str(v) if isinstance(v, str) else '0x%08X' % (v & 0xFFFFFFFF) for v in vals[i:i + 2])
            for i in range(0, len(vals), 2)]

def build_data():
    L = []
    # ---- liste d'affichage F3DEX2
    comb_w0 = 0xFC000000 | 1 << 20 | 4 << 15 | 7 << 12 | 7 << 9 | 1 << 5 | 4
    comb_w1 = 15 << 28 | 15 << 24 | 7 << 21 | 7 << 18 | 7 << 15 | 7 << 12 | 4 << 9 | 7 << 6 | 7 << 3 | 4
    dl = [
        ('', 0xE7000000, 0),                        # pipe sync
        ('', 0xFF10013F, 0x00180000),               # image couleur = tampon Z (effacement)
        ('', 0xE3000A01, 0x00300000),               # cycle FILL
        ('', 0xF7000000, 0xFFFCFFFC),               # couleur = Z maximal
        ('', 0xF64FC3BC, 0x00000000),               # rectangle 0,0 - 319,239
        ('', 0xE7000000, 0),
        ('DLCIMG', 0xFF10013F, 0x00100000),         # image couleur (modifiee a chaque trame)
        ('', 0xFE000000, 0x00180000),               # image Z
        ('', 0xF7000000, 0x10891089),               # fond bleu nuit
        ('', 0xF64FC3BC, 0x00000000),
        ('', 0xE7000000, 0),
        ('', 0xE3000A01, 0x00000000),               # 1 cycle
        ('', 0xE3000C00, 0x00080000),               # correction de perspective
        ('', 0xE3001201, 0x00002000),               # filtrage bilineaire
        ('', 0xE200001F, 0x00000030),               # Z lecture + ecriture, opaque
        ('', 0xFD10001F, 'TEXDATA'),                # image texture RGBA16 32 px
        ('', 0xF5100000, 0x07000000),               # tuile 7 (chargement)
        ('', 0xE6000000, 0),                        # load sync
        ('', 0xF3000000, 0x073FF100),               # LOADBLOCK 1024 texels, dxt 256
        ('', 0xE7000000, 0),
        ('', 0xF5101000, 0x00014050),               # tuile 0 : 32x32, repetition
        ('', 0xF2000000, 0x0007C07C),               # taille de tuile
        ('', 0xD7000002, 0xFFFFFFFF),               # texture active, echelle 1
        ('', comb_w0, comb_w1),                     # TEXEL0 * SHADE
        ('', 0xDA380007, 'PROJ'),                   # projection
        ('DLMV', 0xDA380003, 'MTXTAB'),             # modelview (modifiee a chaque trame)
        ('', 0xDC080008, 'VIEWPORT'),
        ('', 0xD9000000, 0x00200405),               # Z, ombrage lisse, elimination faces arriere
        ('', 0x01018030, 'VERTS'),                  # 24 sommets
    ]
    for f in range(6):
        b = f * 4 * 2
        dl.append(('', 0x06000000 | b << 16 | (b + 2) << 8 | (b + 4), (b) << 16 | (b + 4) << 8 | (b + 6)))
    dl += [('', 0xE9000000, 0), ('', 0xDF000000, 0)]
    L.append('DL:')
    for lab, w0, w1 in dl:
        if lab: L.append(lab + ':')
        L += words([w0, w1])
    # ---- viewport 320x240
    L.append('VIEWPORT:')
    L += halves([640, 480, 511, 0, 640, 480, 511, 0])
    # ---- projection
    L.append('PROJ:')
    L += halves(fixmtx(perspective(55, 4 / 3, 20, 2000)))
    # ---- sommets : 6 faces x 4, sens trigonometrique vu de l'exterieur
    faces = [((0, 0, 1), (255, 90, 90)), ((0, 0, -1), (90, 255, 90)), ((1, 0, 0), (90, 140, 255)),
             ((-1, 0, 0), (255, 230, 80)), ((0, 1, 0), (255, 120, 255)), ((0, -1, 0), (90, 240, 240))]
    S = 70
    L.append('VERTS:')
    for n, col in faces:
        n = list(n)
        # deux axes tangents (u, v) avec u x v = n
        up = [0, 1, 0] if n[1] == 0 else [0, 0, 1]
        u = [up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]]
        v = [n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]]
        corners = [(-1, -1, 0, 1024), (1, -1, 1024, 1024), (1, 1, 1024, 0), (-1, 1, 0, 0)]
        for a, b, s, t in corners:
            p = [S * (n[k] + a * u[k] + b * v[k]) for k in range(3)]
            L += halves([int(p[0]), int(p[1]), int(p[2]), 0, s, t, col[0] << 8 | col[1], col[2] << 8 | 255])
    # ---- texture damier 32x32 RGBA16 avec bordure
    tex = []
    for y in range(32):
        for x in range(32):
            if x < 2 or y < 2 or x > 29 or y > 29: r, g, b = 31, 31, 31
            elif ((x >> 3) ^ (y >> 3)) & 1: r, g, b = 31, 31, 31
            else: r, g, b = 12, 12, 16
            tex.append(r << 11 | g << 6 | b << 1 | 1)
    L.append('TEXDATA:')
    L += halves(tex)
    # ---- chaine d'identification du microcode (lue par l'emulateur)
    txt = b'RSP Gfx ucode F3DEX2 (demo maison)'.ljust(64, b'\0')
    L.append('UCSTR:')
    L += words([struct.unpack('>I', txt[i:i + 4])[0] for i in range(0, 64, 4)])
    # ---- son : liste audio ABI1 (3 passes de 176 echantillons -> 528 en stereo)
    L.append('ALIST:')
    al = [('ASEG', 0x07000000, 0x011C0000)]
    al += [('AVOL', 0x09060000, 0), ('', 0x09040000, 0), ('', 0x09020000, 0x00010000), ('', 0x09000000, 0x00010000),
           ('', 0x09087FFF, 0x00000000)]
    for p in range(3):
        al += [
            ('', 0x08000010, 352),                      # in = 0x010, 352 octets
            ('', 0x04000000, 'WAVE'),                   # LOADBUFF : 2 periodes
            ('', 0x08000010, 0x0240 << 16 | 352),       # out = 0x240
            ('', 0x05008000, 'RSTATE'),                 # RESAMPLE pas 1.0
            ('', 0x020003A0, 352), ('', 0x02000500, 352),   # efface G et D
            ('', 0x08000240, 0x03A0 << 16 | 352),       # in = mono, out = gauche
            ('', 0x08080500, 0x0660 << 16 | 0x0660),    # droite = 0x500 (aux inutilise)
            ('', 0x03010000, 'ESTATE'),                 # ENVMIXER (init)
            ('', 0x08000000, 0x0660 << 16 | 352),       # out = 0x660
            ('', 0x0D000000, 0x03A0 << 16 | 0x0500),    # INTERLEAVE
            ('', 0x08000000, 0x0660 << 16 | 704),
            ('', 0x06000000, 0x01000000 | p * 704),     # SAVEBUFF -> segment 1
        ]
    for lab, w0, w1 in al:
        if lab: L.append(lab + ':')
        L += words([w0, w1])
    L.append('ALISTEND:')
    L.append('AUCODE:')                                 # en-tete reconnu comme ABI1
    L += words([1] + [0] * 11 + [0xF0000F00, 0, 0, 0])
    L.append('RSTATE:')
    L += words([0] * 4)
    L.append('ESTATE:')
    L += words([0] * 20)
    L.append('WAVE:')
    wav = []
    for i in range(176):
        t = 2 * math.pi * i / 88
        wav.append(int(9000 * math.sin(t) + 2500 * math.sin(2 * t) + 1200 * math.sin(3 * t)))
    L += halves(wav)
    # ---- 128 matrices modelview
    L.append('MTXTAB:')
    for i in range(128):
        a = i * 2 * math.pi / 128
        m = mmul(mmul(mmul(rot_y(a * 2), rot_x(a)), rot_x(0.5)), trans(0, 0, -330))
        L += halves(fixmtx(m))
    return '\n'.join(L) + '\n'

def main():
    src = open(os.path.join(HERE, 'demo.s')).read() + build_data()
    code, syms = assemble(src, 0x80000400)
    rom = bytearray(0x1000 + len(code))
    rom[0:24] = struct.pack('>IIIIII', 0x80371240, 0x0000000F, 0x80000400, 0x0000144C, 0, 0)
    rom[0x20:0x34] = b'DEMO N64 MAISON     '
    rom[0x3B:0x3F] = b'NDME'
    rom[0x1000:] = code
    out = ['/* demo_rom.c : ROM de demonstration maison (generee depuis tools/demo.s par',
           ' * tools/make_demo.py). Code original, libre de droits. Cube 3D (etape 3). */',
           '#include <stdint.h>',
           'const uint32_t demo_rom_size = %d;' % len(rom),
           'const uint8_t demo_rom[%d] = {' % len(rom)]
    for i in range(0, len(rom), 16):
        out.append('    ' + ', '.join('0x%02X' % b for b in rom[i:i + 16]) + ',')
    out.append('};')
    open(os.path.join(ROOT, 'source', 'demo_rom.c'), 'w').write('\n'.join(out) + '\n')
    open(os.path.join(ROOT, 'build_host', 'demo.z64'), 'wb').write(bytes(rom)) if os.path.isdir(os.path.join(ROOT, 'build_host')) else None
    print('demo : %d octets, liste a 0x%08X, matrices a 0x%08X' % (len(rom), syms['DL'], syms['MTXTAB']))

if __name__ == '__main__':
    main()
