/* ppc_interp.c : interpreteur PowerPC 64 bits MINIMAL, pour TESTER sur PC le
 * code genere par le recompilateur (source/jit.c). Il ne connait que les
 * instructions que le recompilateur emet ; toute autre instruction arrete le
 * test avec une erreur.
 *  - memoire : adresses de l'hote ; acces big-endian dans la RDRAM emulee
 *    (comme sur PS3), natifs ailleurs (structure VR4300 : le recompilateur n'y
 *    fait que des acces de la taille exacte des champs).
 *  - appels de fonctions C : adresses factices 0x7F000000 + 16 * n. */
#include "ppc_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PpcHelper helpers[16];
static int nhelpers;
static uint8_t *be_lo, *be_hi;           /* zone big-endian (RDRAM) */
uint64_t ppc_executed;

uint64_t ppc_register_helper(PpcHelper f)
{
    helpers[nhelpers] = f;
    return 0x7F000000u + 16u * (uint32_t)nhelpers++;
}

void ppc_set_bigendian_range(uint8_t *lo, uint32_t size) { be_lo = lo; be_hi = lo + size; }

static int in_be(uint64_t a) { return (uint8_t *)(uintptr_t)a >= be_lo && (uint8_t *)(uintptr_t)a < be_hi; }

static uint64_t rd(uint64_t a, int n)
{
    uint8_t *p = (uint8_t *)(uintptr_t)a;
    uint64_t v = 0;
    int i;
    if (in_be(a)) { for (i = 0; i < n; i++) v = v << 8 | p[i]; return v; }
    switch (n) {
    case 1: return *p;
    case 2: { uint16_t x; memcpy(&x, p, 2); return x; }
    case 4: { uint32_t x; memcpy(&x, p, 4); return x; }
    default: { uint64_t x; memcpy(&x, p, 8); return x; }
    }
}

static void wr(uint64_t a, int n, uint64_t v)
{
    uint8_t *p = (uint8_t *)(uintptr_t)a;
    int i;
    if (in_be(a)) { for (i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } return; }
    switch (n) {
    case 1: *p = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    default: memcpy(p, &v, 8); break;
    }
}

static uint64_t rotl64(uint64_t v, int n) { n &= 63; return n ? (v << n) | (v >> (64 - n)) : v; }
static uint64_t mask64(int mb, int me)   /* numerotation IBM : bit 0 = poids fort */
{
    uint64_t m = 0;
    int i = mb;
    for (;;) { m |= 1ull << (63 - i); if (i == me) break; i = (i + 1) & 63; }
    return m;
}

static void set_cr0(uint32_t *cr, int lt, int gt, int eq)
{
    *cr = (*cr & 0x0FFFFFFFu) | (uint32_t)lt << 31 | (uint32_t)gt << 30 | (uint32_t)eq << 29;
}

static void set_cr(uint32_t *cr, int bf, int lt, int gt, int eq)
{
    uint32_t f = (uint32_t)lt << 3 | (uint32_t)gt << 2 | (uint32_t)eq << 1;
    int sh = 28 - 4 * bf;
    *cr = (*cr & ~(0xFu << sh)) | f << sh;
}

#define EXT16(x) ((int64_t)(int16_t)(x))

uint32_t ppc_call(uint64_t entry, uint64_t arg0, int *error)
{
    static uint8_t stack[1 << 16];
    uint64_t r[32] = { 0 }, lr = 0xDEAD0000u, ctr = 0, pc = entry;
    uint32_t cr = 0;
    uint64_t guard = 0;
    r[1] = (uint64_t)(uintptr_t)(stack + sizeof stack - 256);
    r[3] = arg0;
    *error = 0;
    while (pc != 0xDEAD0000u) {
        uint32_t ins, op, rt, ra, rb, xo;
        uint64_t npc;
        if (pc >= 0x7F000000u && pc < 0x7F000000u + 16u * 16u) {        /* appel d'une aide C */
            int k = (int)((pc - 0x7F000000u) / 16);
            r[3] = (uint64_t)helpers[k]((void *)(uintptr_t)r[3], (uint32_t)r[4], (uint32_t)r[5]);
            pc = lr;
            continue;
        }
        if (++guard > 50000000ull) { *error = 2; return 0; }
        memcpy(&ins, (void *)(uintptr_t)pc, 4);
        ppc_executed++;
        op = ins >> 26; rt = (ins >> 21) & 31; ra = (ins >> 16) & 31; rb = (ins >> 11) & 31;
        npc = pc + 4;
#define RA0 (ra ? r[ra] : 0)
        switch (op) {
        case 10: {                                                      /* cmpli */
            int bf = (int)(rt >> 2), L = (int)(rt & 1);
            uint64_t a = L ? r[ra] : (uint32_t)r[ra], b = ins & 0xFFFF;
            set_cr(&cr, bf, a < b, a > b, a == b);
            break;
        }
        case 11: {                                                      /* cmpi */
            int bf = (int)(rt >> 2), L = (int)(rt & 1);
            int64_t a = L ? (int64_t)r[ra] : (int64_t)(int32_t)r[ra], b = EXT16(ins);
            set_cr(&cr, bf, a < b, a > b, a == b);
            break;
        }
        case 14: r[rt] = RA0 + (uint64_t)EXT16(ins); break;             /* addi */
        case 15: r[rt] = RA0 + (uint64_t)(EXT16(ins) * 65536); break;     /* addis */
        case 16: {                                                      /* bc */
            int bo = (int)rt, bi = (int)ra, ctr_ok, cond_ok;
            if (!(bo & 4)) ctr--;
            ctr_ok = (bo & 4) || ((ctr != 0) ^ ((bo >> 1) & 1));
            cond_ok = (bo & 16) || (((cr >> (31 - bi)) & 1) == (uint32_t)((bo >> 3) & 1));
            if (ins & 1) lr = pc + 4;
            if (ctr_ok && cond_ok) npc = pc + (uint64_t)(int64_t)(int16_t)(ins & 0xFFFC);
            break;
        }
        case 18: {                                                      /* b */
            int32_t d = (int32_t)(ins & 0x3FFFFFC);
            if (d & 0x2000000) d -= 0x4000000;
            if (ins & 1) lr = pc + 4;
            npc = pc + (uint64_t)(int64_t)d;
            break;
        }
        case 19:
            xo = (ins >> 1) & 1023;
            if (xo == 16 && rt == 20) { npc = lr; if (ins & 1) lr = pc + 4; }
            else if (xo == 528 && rt == 20) { uint64_t t = ctr; if (ins & 1) lr = pc + 4; npc = t; }
            else { *error = 1; fprintf(stderr, "ppc: op19 %08X inconnu\n", ins); return 0; }
            break;
        case 21: {                                                      /* rlwinm */
            int sh = (int)rb, mb = (int)((ins >> 6) & 31), me = (int)((ins >> 1) & 31);
            uint32_t w = (uint32_t)r[rt];
            uint64_t rot = rotl64((uint64_t)w << 32 | w, sh);
            r[ra] = rot & mask64(mb + 32, me + 32);
            if (ins & 1) set_cr0(&cr, (int64_t)r[ra] < 0, (int64_t)r[ra] > 0, r[ra] == 0);
            break;
        }
        case 24: r[ra] = r[rt] | (ins & 0xFFFF); break;
        case 25: r[ra] = r[rt] | (uint64_t)(ins & 0xFFFF) << 16; break;
        case 26: r[ra] = r[rt] ^ (ins & 0xFFFF); break;
        case 28: r[ra] = r[rt] & (ins & 0xFFFF);                        /* andi. */
            set_cr0(&cr, (int64_t)r[ra] < 0, (int64_t)r[ra] > 0, r[ra] == 0); break;
        case 29: r[ra] = r[rt] & ((uint64_t)(ins & 0xFFFF) << 16);      /* andis. */
            set_cr0(&cr, (int64_t)r[ra] < 0, (int64_t)r[ra] > 0, r[ra] == 0); break;
        case 30: {                                                      /* rldicl / rldicr */
            int sh = (int)rb | (int)((ins >> 1) & 1) << 5;
            int m6 = (int)((ins >> 5) & 63), m = ((m6 & 1) << 5) | (m6 >> 1);
            int x3 = (int)((ins >> 2) & 7);
            uint64_t rot = rotl64(r[rt], sh);
            if (x3 == 0) r[ra] = rot & mask64(m, 63);
            else if (x3 == 1) r[ra] = rot & mask64(0, m);
            else if (x3 == 3) { uint64_t mk = mask64(m, 63 - sh); r[ra] = (rot & mk) | (r[ra] & ~mk); }
            else { *error = 1; fprintf(stderr, "ppc: MD %08X inconnu\n", ins); return 0; }
            break;
        }
        case 31:
            xo = (ins >> 1) & 1023;
            if (((ins >> 2) & 511) == 413) {                            /* sradi */
                int sh = (int)rb | (int)((ins >> 1) & 1) << 5;
                r[ra] = (uint64_t)((int64_t)r[rt] >> sh);
                break;
            }
            switch (xo) {
            case 0: case 32: {                                          /* cmp / cmpl */
                int bf = (int)(rt >> 2), L = (int)(rt & 1);
                if (xo == 0) {
                    int64_t a = L ? (int64_t)r[ra] : (int64_t)(int32_t)r[ra], b = L ? (int64_t)r[rb] : (int64_t)(int32_t)r[rb];
                    set_cr(&cr, bf, a < b, a > b, a == b);
                } else {
                    uint64_t a = L ? r[ra] : (uint32_t)r[ra], b = L ? r[rb] : (uint32_t)r[rb];
                    set_cr(&cr, bf, a < b, a > b, a == b);
                }
                break;
            }
            case 266: r[rt] = r[ra] + r[rb]; break;
            case 233: r[rt] = r[ra] * r[rb]; break;
            case 40: r[rt] = r[rb] - r[ra]; break;
            case 28: r[ra] = r[rt] & r[rb]; break;
            case 444: r[ra] = r[rt] | r[rb]; break;
            case 316: r[ra] = r[rt] ^ r[rb]; break;
            case 124: r[ra] = ~(r[rt] | r[rb]); break;
            case 986: r[ra] = (uint64_t)(int64_t)(int32_t)r[rt]; break;
            case 954: r[ra] = (uint64_t)(int64_t)(int8_t)r[rt]; break;
            case 922: r[ra] = (uint64_t)(int64_t)(int16_t)r[rt]; break;
            case 24: { int n = (int)(r[rb] & 63); r[ra] = n >= 32 ? 0 : (uint32_t)((uint32_t)r[rt] << n); break; }
            case 536: { int n = (int)(r[rb] & 63); r[ra] = n >= 32 ? 0 : ((uint32_t)r[rt] >> n); break; }
            case 792: { int n = (int)(r[rb] & 63); int32_t v = (int32_t)r[rt]; r[ra] = (uint64_t)(int64_t)(n >= 32 ? (v < 0 ? -1 : 0) : v >> n); break; }
            case 824: { int32_t v = (int32_t)r[rt]; r[ra] = (uint64_t)(int64_t)(v >> rb); break; }
            case 27: { int n = (int)(r[rb] & 127); r[ra] = n >= 64 ? 0 : r[rt] << n; break; }
            case 539: { int n = (int)(r[rb] & 127); r[ra] = n >= 64 ? 0 : r[rt] >> n; break; }
            case 794: { int n = (int)(r[rb] & 127); int64_t v = (int64_t)r[rt]; r[ra] = (uint64_t)(n >= 64 ? (v < 0 ? -1 : 0) : v >> n); break; }
            case 23: r[rt] = rd(RA0 + r[rb], 4); break;
            case 87: r[rt] = rd(RA0 + r[rb], 1); break;
            case 279: r[rt] = rd(RA0 + r[rb], 2); break;
            case 343: r[rt] = (uint64_t)(int64_t)(int16_t)rd(RA0 + r[rb], 2); break;
            case 21: r[rt] = rd(RA0 + r[rb], 8); break;
            case 151: wr(RA0 + r[rb], 4, r[rt]); break;
            case 215: wr(RA0 + r[rb], 1, r[rt]); break;
            case 407: wr(RA0 + r[rb], 2, r[rt]); break;
            case 149: wr(RA0 + r[rb], 8, r[rt]); break;
            case 467: {                                                 /* mtspr */
                int spr = (int)(ra | rb << 5);
                if (spr == 9) ctr = r[rt]; else if (spr == 8) lr = r[rt];
                else { *error = 1; return 0; }
                break;
            }
            case 339: {                                                 /* mfspr */
                int spr = (int)(ra | rb << 5);
                if (spr == 8) r[rt] = lr; else if (spr == 9) r[rt] = ctr;
                else { *error = 1; return 0; }
                break;
            }
            default: *error = 1; fprintf(stderr, "ppc: op31/%u inconnu (%08X)\n", xo, ins); return 0;
            }
            break;
        case 32: r[rt] = rd(RA0 + (uint64_t)EXT16(ins), 4); break;
        case 34: r[rt] = rd(RA0 + (uint64_t)EXT16(ins), 1); break;
        case 36: wr(RA0 + (uint64_t)EXT16(ins), 4, r[rt]); break;
        case 38: wr(RA0 + (uint64_t)EXT16(ins), 1, r[rt]); break;
        case 40: r[rt] = rd(RA0 + (uint64_t)EXT16(ins), 2); break;
        case 42: r[rt] = (uint64_t)(int64_t)(int16_t)rd(RA0 + (uint64_t)EXT16(ins), 2); break;
        case 44: wr(RA0 + (uint64_t)EXT16(ins), 2, r[rt]); break;
        case 58:
            if ((ins & 3) != 0) { *error = 1; return 0; }
            r[rt] = rd(RA0 + (uint64_t)EXT16(ins & ~3u), 8); break;
        case 62: {
            uint64_t ea = RA0 + (uint64_t)EXT16(ins & ~3u);
            if ((ins & 3) == 0) wr(ea, 8, r[rt]);
            else if ((ins & 3) == 1) { wr(ea, 8, r[rt]); r[ra] = ea; }
            else { *error = 1; return 0; }
            break;
        }
        default: *error = 1; fprintf(stderr, "ppc: op %u inconnu (%08X)\n", op, ins); return 0;
        }
        pc = npc;
    }
    return (uint32_t)r[3];
}
