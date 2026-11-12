/* vr4300.c : interpreteur MIPS III du VR4300. */
#include "vr4300.h"
#include <math.h>
#include <string.h>

#define SE32(x)  ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))
#define SE16(x)  ((uint64_t)(int64_t)(int16_t)(uint16_t)(x))
#define SE8(x)   ((uint64_t)(int64_t)(int8_t)(uint8_t)(x))

#define ST_IE  0x01
#define ST_EXL 0x02
#define ST_ERL 0x04
#define ST_FR  0x04000000
#define ST_BEV 0x00400000

#define FCSR_C 0x00800000

/* ------------------------------------------------------------------ RDRAM */

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HOST_BE 1
#else
#define HOST_BE 0
#endif

static inline uint32_t ld32(const uint8_t *p)
{
#if HOST_BE
    return *(const uint32_t *)p;
#else
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
#endif
}

static inline void st32(uint8_t *p, uint32_t v)
{
#if HOST_BE
    *(uint32_t *)p = v;
#else
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
#endif
}

static uint64_t phys_read(VR4300 *c, uint32_t pa, int size)
{
    if (pa + (uint32_t)size <= c->rdram_size) {
        const uint8_t *p = c->rdram + pa;
        switch (size) {
        case 1: return p[0];
        case 2: return (uint16_t)(p[0] << 8 | p[1]);
        case 4: return ld32(p);
        default: return (uint64_t)ld32(p) << 32 | ld32(p + 4);
        }
    }
    return c->read(c->ctx, pa, size);
}

static void phys_write(VR4300 *c, uint32_t pa, int size, uint64_t v)
{
    if (pa + (uint32_t)size <= c->rdram_size) {
        uint8_t *p = c->rdram + pa;
        if (c->codepage && c->codepage[pa >> 12] && c->code_written) c->code_written(c, pa);
        switch (size) {
        case 1: p[0] = (uint8_t)v; break;
        case 2: p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; break;
        case 4: st32(p, (uint32_t)v); break;
        default: st32(p, (uint32_t)(v >> 32)); st32(p + 4, (uint32_t)v); break;
        }
        return;
    }
    c->write(c->ctx, pa, size, v);
}

/* ------------------------------------------------------------------ exceptions */

static void exception(VR4300 *c, int code, uint32_t vector_off)
{
    uint32_t epc = c->pc;                     /* pc = adresse de l'instruction fautive */
    uint64_t status = c->cp0[CP0_STATUS];
    c->exceptions++;
    c->last_exception = (uint32_t)code;
    if (!(status & ST_EXL)) {
        if (c->in_delay) { epc -= 4; c->cp0[CP0_CAUSE] |= 0x80000000u; }
        else c->cp0[CP0_CAUSE] &= ~(uint64_t)0x80000000u;
        c->cp0[CP0_EPC] = SE32(epc);
    } else {
        vector_off = 0x180;
    }
    c->cp0[CP0_CAUSE] = (c->cp0[CP0_CAUSE] & ~(uint64_t)0x7C) | (uint64_t)((code & 31) << 2);
    c->cp0[CP0_STATUS] |= ST_EXL;
    c->pc = ((status & ST_BEV) ? 0xBFC00200u : 0x80000000u) + vector_off;
    c->next_pc = c->pc + 4;
    c->branch_pending = 0;
    c->in_delay = 0;
}

static void address_error(VR4300 *c, uint32_t vaddr, int write)
{
    c->cp0[CP0_BADVADDR] = SE32(vaddr);
    exception(c, write ? EXC_ADES : EXC_ADEL, 0x180);
}

/* ------------------------------------------------------------------ TLB */

static int tlb_lookup(VR4300 *c, uint32_t vaddr, int write, uint32_t *pa, int *code, int *refill)
{
    uint8_t asid = (uint8_t)c->cp0[CP0_ENTRYHI];
    int i;
    for (i = 0; i < 32; i++) {
        const TlbEntry *e = &c->tlb[i];
        uint32_t mask = (e->mask | 0x1FFF);           /* masque VPN2 (sans le bit pair/impair) */
        uint32_t vpn2 = (uint32_t)e->hi & ~mask;
        if ((vaddr & ~mask) != vpn2) continue;
        if (!e->global && (uint8_t)e->hi != asid) continue;
        {
            uint32_t odd_bit = (mask + 1) >> 1;
            uint64_t lo = e->lo[(vaddr & odd_bit) ? 1 : 0];
            uint32_t page_mask = mask >> 1;
            if (!(lo & 2)) { *code = write ? EXC_TLBS : EXC_TLBL; *refill = 0; return -1; }  /* invalide */
            if (write && !(lo & 4)) { *code = EXC_MOD; *refill = 0; return -1; }            /* non modifiable */
            *pa = (uint32_t)(((lo >> 6) & 0xFFFFF) << 12 & ~page_mask) | (vaddr & page_mask);
            return 0;
        }
    }
    *code = write ? EXC_TLBS : EXC_TLBL;
    *refill = 1;
    return -1;
}

int vr4300_virt_to_phys(VR4300 *c, uint32_t vaddr, int write, uint32_t *paddr)
{
    int code, refill;
    if ((vaddr & 0xC0000000u) == 0x80000000u) { *paddr = vaddr & 0x1FFFFFFF; return 0; }  /* KSEG0/1 */
    if (tlb_lookup(c, vaddr, write, paddr, &code, &refill) == 0) return 0;
    c->cp0[CP0_BADVADDR] = SE32(vaddr);
    c->cp0[CP0_CONTEXT] = (c->cp0[CP0_CONTEXT] & ~(uint64_t)0x7FFFF0) | ((vaddr >> 9) & 0x7FFFF0);
    c->cp0[CP0_ENTRYHI] = (c->cp0[CP0_ENTRYHI] & 0xFF) | SE32(vaddr & 0xFFFFE000u);
    exception(c, code, (refill && !(c->cp0[CP0_STATUS] & ST_EXL)) ? 0x000 : 0x180);
    return -1;
}

static int load(VR4300 *c, uint32_t va, int size, uint64_t *out)
{
    uint32_t pa;
    /* chemin rapide : KSEG0/KSEG1 en RDRAM, aligne (l'immense majorite des acces) */
    if ((va & 0xC0000000u) == 0x80000000u && !(va & (uint32_t)(size - 1)) && (pa = va & 0x1FFFFFFF) + (uint32_t)size <= c->rdram_size) {
        const uint8_t *p = c->rdram + pa;
        switch (size) {
        case 1: *out = p[0]; return 0;
        case 2: *out = (uint64_t)(p[0] << 8 | p[1]); return 0;
        case 4: *out = ld32(p); return 0;
        default: *out = (uint64_t)ld32(p) << 32 | ld32(p + 4); return 0;
        }
    }
    if (va & (uint32_t)(size - 1)) { address_error(c, va, 0); return -1; }
    if (vr4300_virt_to_phys(c, va, 0, &pa)) return -1;
    *out = phys_read(c, pa, size);
    return 0;
}

static int store(VR4300 *c, uint32_t va, int size, uint64_t v)
{
    uint32_t pa;
    if ((va & 0xC0000000u) == 0x80000000u && !(va & (uint32_t)(size - 1)) && (pa = va & 0x1FFFFFFF) + (uint32_t)size <= c->rdram_size) {
        uint8_t *p = c->rdram + pa;
        if (c->codepage && c->codepage[pa >> 12] && c->code_written) c->code_written(c, pa);
        switch (size) {
        case 1: p[0] = (uint8_t)v; return 0;
        case 2: p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return 0;
        case 4: st32(p, (uint32_t)v); return 0;
        default: st32(p, (uint32_t)(v >> 32)); st32(p + 4, (uint32_t)v); return 0;
        }
    }
    if (va & (uint32_t)(size - 1)) { address_error(c, va, 1); return -1; }
    if (vr4300_virt_to_phys(c, va, 1, &pa)) return -1;
    phys_write(c, pa, size, v);
    return 0;
}

/* ------------------------------------------------------------------ COP0 */

void vr4300_set_ip(VR4300 *c, int line, int on)
{
    uint64_t bit = (uint64_t)1 << (8 + line);
    if (on) c->cp0[CP0_CAUSE] |= bit; else c->cp0[CP0_CAUSE] &= ~bit;
}

void vr4300_reset(VR4300 *c)
{
    memset(c->r, 0, sizeof c->r);
    memset(c->cp0, 0, sizeof c->cp0);
    memset(c->fpr, 0, sizeof c->fpr);
    memset(c->tlb, 0, sizeof c->tlb);
    c->hi = c->lo = 0;
    c->fcr31 = 0;
    c->llbit = 0;
    c->cp0[CP0_RANDOM] = 31;
    c->cp0[CP0_STATUS] = 0x34000000;
    c->cp0[CP0_PRID] = 0x00000B22;
    c->cp0[CP0_CONFIG] = 0x0006E463;
    c->pc = 0xBFC00000u;
    c->next_pc = c->pc + 4;
    c->in_delay = c->branch_pending = 0;
    if (c->cpi == 0) c->cpi = 1;
    memset(c->idle_cache, 0, sizeof c->idle_cache);
}

static void tlb_write(VR4300 *c, int idx)
{
    TlbEntry *e = &c->tlb[idx & 31];
    e->mask = (uint32_t)c->cp0[CP0_PAGEMASK] & 0x01FFE000u;
    e->hi = c->cp0[CP0_ENTRYHI] & ~(uint64_t)(e->mask | 0x1F00);
    e->lo[0] = c->cp0[CP0_ENTRYLO0] & ~(uint64_t)1;
    e->lo[1] = c->cp0[CP0_ENTRYLO1] & ~(uint64_t)1;
    e->global = (int)(c->cp0[CP0_ENTRYLO0] & c->cp0[CP0_ENTRYLO1] & 1);
}

static void cop0(VR4300 *c, uint32_t ins)
{
    int rs = (ins >> 21) & 31, rt = (ins >> 16) & 31, rd = (ins >> 11) & 31;
    switch (rs) {
    case 0x00: c->r[rt] = SE32(rd == CP0_RANDOM ? (31 - (c->instructions % (32 - (c->cp0[CP0_WIRED] & 31)))) : c->cp0[rd]); break; /* MFC0 */
    case 0x01: c->r[rt] = c->cp0[rd]; break;                                 /* DMFC0 */
    case 0x04: case 0x05: {                                                  /* MTC0 / DMTC0 */
        uint64_t v = rs == 4 ? SE32(c->r[rt]) : c->r[rt];
        switch (rd) {
        case CP0_COMPARE: c->cp0[rd] = (uint32_t)v; c->cp0[CP0_CAUSE] &= ~(uint64_t)0x8000; break;
        case CP0_COUNT: c->cp0[rd] = (uint32_t)v; break;
        case CP0_CAUSE: c->cp0[rd] = (c->cp0[rd] & ~(uint64_t)0x300) | (v & 0x300); break;
        case CP0_RANDOM: case CP0_PRID: case CP0_BADVADDR: break;
        case CP0_WIRED: c->cp0[rd] = v & 31; c->cp0[CP0_RANDOM] = 31; break;
        default: c->cp0[rd] = v;
        }
        break;
    }
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (ins & 63) {
        case 0x01: {                                                          /* TLBR */
            TlbEntry *e = &c->tlb[c->cp0[CP0_INDEX] & 31];
            c->cp0[CP0_PAGEMASK] = e->mask;
            c->cp0[CP0_ENTRYHI] = e->hi;
            c->cp0[CP0_ENTRYLO0] = e->lo[0] | (uint64_t)e->global;
            c->cp0[CP0_ENTRYLO1] = e->lo[1] | (uint64_t)e->global;
            break;
        }
        case 0x02: tlb_write(c, (int)(c->cp0[CP0_INDEX] & 31)); break;      /* TLBWI */
        case 0x06: {                                                          /* TLBWR */
            int wired = (int)(c->cp0[CP0_WIRED] & 31);
            int idx = wired + (int)(c->instructions % (uint64_t)(32 - wired));
            tlb_write(c, idx);
            break;
        }
        case 0x08: {                                                          /* TLBP */
            int i;
            uint64_t hi = c->cp0[CP0_ENTRYHI];
            c->cp0[CP0_INDEX] = 0x80000000u;
            for (i = 0; i < 32; i++) {
                const TlbEntry *e = &c->tlb[i];
                uint64_t m = ~(uint64_t)(e->mask | 0x1FFF);
                if (((e->hi & m) == (hi & m)) && (e->global || (uint8_t)e->hi == (uint8_t)hi)) {
                    c->cp0[CP0_INDEX] = (uint64_t)i;
                    break;
                }
            }
            break;
        }
        case 0x18:                                                            /* ERET */
            if (c->cp0[CP0_STATUS] & ST_ERL) { c->next_pc = (uint32_t)c->cp0[CP0_ERROREPC]; c->cp0[CP0_STATUS] &= ~(uint64_t)ST_ERL; }
            else { c->next_pc = (uint32_t)c->cp0[CP0_EPC]; c->cp0[CP0_STATUS] &= ~(uint64_t)ST_EXL; }
            c->llbit = 0;
            c->branch_pending = 4;         /* ERET : saut sans creneau de retard */
            break;
        }
        break;
    }
}

/* ------------------------------------------------------------------ COP1 (FPU) */

static float  fs_get(VR4300 *c, int r)
{
    uint32_t bits;
    float f;
    if (c->cp0[CP0_STATUS] & ST_FR) bits = (uint32_t)c->fpr[r];
    else bits = (r & 1) ? (uint32_t)(c->fpr[r & ~1] >> 32) : (uint32_t)c->fpr[r];
    memcpy(&f, &bits, 4);
    return f;
}

static void fs_set_bits(VR4300 *c, int r, uint32_t bits)
{
    if (c->cp0[CP0_STATUS] & ST_FR) c->fpr[r] = (c->fpr[r] & 0xFFFFFFFF00000000ull) | bits;
    else if (r & 1) c->fpr[r & ~1] = (c->fpr[r & ~1] & 0xFFFFFFFFull) | (uint64_t)bits << 32;
    else c->fpr[r] = (c->fpr[r] & 0xFFFFFFFF00000000ull) | bits;
}

static void fs_set(VR4300 *c, int r, float f) { uint32_t b; memcpy(&b, &f, 4); fs_set_bits(c, r, b); }

static uint64_t fd_bits(VR4300 *c, int r) { return (c->cp0[CP0_STATUS] & ST_FR) ? c->fpr[r] : c->fpr[r & ~1]; }
static void fd_set_bits(VR4300 *c, int r, uint64_t b) { if (c->cp0[CP0_STATUS] & ST_FR) c->fpr[r] = b; else c->fpr[r & ~1] = b; }
static double fd_get(VR4300 *c, int r) { uint64_t b = fd_bits(c, r); double d; memcpy(&d, &b, 8); return d; }
static void fd_set(VR4300 *c, int r, double d) { uint64_t b; memcpy(&b, &d, 8); fd_set_bits(c, r, b); }

static double round_mode(double x, int mode)
{
    switch (mode & 3) {
    case 0:                                 /* au plus proche, egalite -> pair */
        /* astuce 1,5 x 2^52 : l'addition arrondit au pair le plus proche, sans appel a la libm */
        if (x > -2251799813685248.0 && x < 2251799813685248.0) {
            volatile double t = x + 6755399441055744.0;
            return t - 6755399441055744.0;
        }
        return x;
    case 1: return trunc(x);
    case 2: return ceil(x);
    default: return floor(x);
    }
}

static int32_t to_w(double x) { if (x != x) return INT32_MAX; if (x >= 2147483647.0) return INT32_MAX; if (x <= -2147483648.0) return INT32_MIN; return (int32_t)x; }
static int64_t to_l(double x) { if (x != x) return INT64_MAX; if (x >= 9.2233720368547758e18) return INT64_MAX; if (x <= -9.2233720368547758e18) return INT64_MIN; return (int64_t)x; }

static void cop1(VR4300 *c, uint32_t ins)
{
    int fmt = (ins >> 21) & 31, ft = (ins >> 16) & 31, fs = (ins >> 11) & 31, fd = (ins >> 6) & 31, fn = ins & 63;
    int rt = ft;
    switch (fmt) {
    case 0x00: c->r[rt] = SE32(fd_bits(c, fs & 31) >> (((c->cp0[CP0_STATUS] & ST_FR) || !(fs & 1)) ? 0 : 32)); return; /* MFC1 */
    case 0x01: c->r[rt] = fd_bits(c, fs); return;                                          /* DMFC1 */
    case 0x02: c->r[rt] = SE32(fs == 31 ? c->fcr31 : fs == 0 ? 0x00000B00u : 0); return;    /* CFC1 */
    case 0x04: fs_set_bits(c, fs, (uint32_t)c->r[rt]); return;                             /* MTC1 */
    case 0x05: fd_set_bits(c, fs, c->r[rt]); return;                                       /* DMTC1 */
    case 0x06: if (fs == 31) c->fcr31 = (uint32_t)c->r[rt] & 0x0183FFFFu; return;          /* CTC1 */
    case 0x08: {                                                                           /* BC1 */
        int cond = (c->fcr31 & FCSR_C) != 0, tf = ft & 1, likely = (ft >> 1) & 1;
        if (cond == tf) { c->next_pc = c->pc + 4 + ((uint32_t)(int32_t)(int16_t)ins << 2); c->branch_pending = 1; }
        else if (likely) c->branch_pending = 3;
        else { c->next_pc = c->pc + 8; c->branch_pending = 1; }
        return;
    }
    }
    if (fmt == 0x10 || fmt == 0x11) {
        int dbl = fmt == 0x11;
        double a = dbl ? fd_get(c, fs) : fs_get(c, fs);
        double b = dbl ? fd_get(c, ft) : fs_get(c, ft);
        double r = 0;
        int set = 1;
        switch (fn) {
        case 0x00: r = dbl ? a + b : (double)((float)a + (float)b); break;
        case 0x01: r = dbl ? a - b : (double)((float)a - (float)b); break;
        case 0x02: r = dbl ? a * b : (double)((float)a * (float)b); break;
        case 0x03: r = dbl ? a / b : (double)((float)a / (float)b); break;
        case 0x04: r = dbl ? sqrt(a) : (double)sqrtf((float)a); break;
        case 0x05: r = fabs(a); break;
        case 0x06: r = a; break;
        case 0x07: r = -a; break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: {                 /* ROUND/TRUNC/CEIL/FLOOR.L */
            static const int m[4] = { 0, 1, 2, 3 };
            fd_set_bits(c, fd, (uint64_t)to_l(round_mode(a, m[fn & 3])));
            return;
        }
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: {                 /* ROUND/TRUNC/CEIL/FLOOR.W */
            static const int m[4] = { 0, 1, 2, 3 };
            fs_set_bits(c, fd, (uint32_t)to_w(round_mode(a, m[fn & 3])));
            return;
        }
        case 0x20: fs_set(c, fd, (float)a); return;                   /* CVT.S */
        case 0x21: fd_set(c, fd, a); return;                          /* CVT.D */
        case 0x24: fs_set_bits(c, fd, (uint32_t)to_w(round_mode(a, c->fcr31 & 3))); return;   /* CVT.W */
        case 0x25: fd_set_bits(c, fd, (uint64_t)to_l(round_mode(a, c->fcr31 & 3))); return;   /* CVT.L */
        default:
            if (fn >= 0x30) {                                         /* C.cond */
                int cond = fn & 15, res = 0;
                int un = (a != a) || (b != b);
                if (un) res = cond & 1;
                else res = ((cond & 2) && a == b) || ((cond & 4) && a < b);
                if (res) c->fcr31 |= FCSR_C; else c->fcr31 &= ~FCSR_C;
                return;
            }
            set = 0;
        }
        if (set) { if (dbl) fd_set(c, fd, r); else fs_set(c, fd, (float)r); }
        return;
    }
    if (fmt == 0x14 || fmt == 0x15) {                                 /* W / L -> S / D */
        double v;
        if (fmt == 0x14) { int32_t w; uint32_t b = (c->cp0[CP0_STATUS] & ST_FR) ? (uint32_t)c->fpr[fs] : ((fs & 1) ? (uint32_t)(c->fpr[fs & ~1] >> 32) : (uint32_t)c->fpr[fs]); w = (int32_t)b; v = w; }
        else v = (double)(int64_t)fd_bits(c, fs);
        if (fn == 0x20) fs_set(c, fd, (float)v);
        else if (fn == 0x21) fd_set(c, fd, v);
    }
}

/* ------------------------------------------------------------------ multiplication 64x64 */

static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (mid << 32) | (uint32_t)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* ------------------------------------------------------------------ boucle */

#define RS (c->r[(ins >> 21) & 31])
#define RT (c->r[(ins >> 16) & 31])
#define RD (c->r[(ins >> 11) & 31])
#define IMM ((int16_t)ins)
#define SA ((ins >> 6) & 31)
#define VADDR ((uint32_t)(RS + (uint64_t)(int64_t)IMM))

static void branch(VR4300 *c, int cond, uint32_t ins, int likely, int link)
{
    if (link) c->r[31] = SE32(c->pc + 8);
    if (cond) { c->next_pc = c->pc + 4 + ((uint32_t)(int32_t)IMM << 2); c->branch_pending = 1; }
    else if (likely) c->branch_pending = 3;                 /* annule le creneau de retard */
    else { c->next_pc = c->pc + 8; c->branch_pending = 1; }
}

static int check_interrupts(VR4300 *c)
{
    uint64_t st = c->cp0[CP0_STATUS];
    if ((st & ST_IE) && !(st & (ST_EXL | ST_ERL)) && (c->cp0[CP0_CAUSE] & st & 0xFF00)) {
        exception(c, EXC_INT, 0x180);
        return 1;
    }
    return 0;
}

static void execute(VR4300 *c, uint32_t ins)
{
    uint64_t v;
    uint32_t a;
    int op = ins >> 26;

    switch (op) {
    case 0x00: {                                                         /* SPECIAL */
        switch (ins & 63) {
        case 0x00: RD = SE32((uint32_t)RT << SA); break;                 /* SLL */
        case 0x02: RD = SE32((uint32_t)RT >> SA); break;                 /* SRL */
        case 0x03: RD = SE32((int32_t)(uint32_t)RT >> SA); break;        /* SRA */
        case 0x04: RD = SE32((uint32_t)RT << (RS & 31)); break;          /* SLLV */
        case 0x06: RD = SE32((uint32_t)RT >> (RS & 31)); break;          /* SRLV */
        case 0x07: RD = SE32((int32_t)(uint32_t)RT >> (RS & 31)); break; /* SRAV */
        case 0x08: c->next_pc = (uint32_t)RS; c->branch_pending = 1; break;                       /* JR */
        case 0x09: { uint32_t t = (uint32_t)RS; RD = SE32(c->pc + 8); c->next_pc = t; c->branch_pending = 1; break; } /* JALR */
        case 0x0C: exception(c, EXC_SYS, 0x180); return;                 /* SYSCALL */
        case 0x0D: exception(c, EXC_BP, 0x180); return;                  /* BREAK */
        case 0x0F: break;                                                /* SYNC */
        case 0x10: RD = c->hi; break;
        case 0x11: c->hi = RS; break;
        case 0x12: RD = c->lo; break;
        case 0x13: c->lo = RS; break;
        case 0x14: RD = RT << (RS & 63); break;                          /* DSLLV */
        case 0x16: RD = RT >> (RS & 63); break;                          /* DSRLV */
        case 0x17: RD = (uint64_t)((int64_t)RT >> (RS & 63)); break;     /* DSRAV */
        case 0x18: { int64_t p = (int64_t)(int32_t)RS * (int32_t)RT; c->lo = SE32(p); c->hi = SE32(p >> 32); break; } /* MULT */
        case 0x19: { uint64_t p = (uint64_t)(uint32_t)RS * (uint32_t)RT; c->lo = SE32(p); c->hi = SE32(p >> 32); break; } /* MULTU */
        case 0x1A: {                                                     /* DIV */
            int32_t n = (int32_t)RS, d = (int32_t)RT;
            if (d == 0) { c->lo = n < 0 ? 1 : SE32(0xFFFFFFFFu); c->hi = SE32(n); }
            else if (n == INT32_MIN && d == -1) { c->lo = SE32(n); c->hi = 0; }
            else { c->lo = SE32(n / d); c->hi = SE32(n % d); }
            break;
        }
        case 0x1B: {                                                     /* DIVU */
            uint32_t n = (uint32_t)RS, d = (uint32_t)RT;
            if (d == 0) { c->lo = SE32(0xFFFFFFFFu); c->hi = SE32(n); }
            else { c->lo = SE32(n / d); c->hi = SE32(n % d); }
            break;
        }
        case 0x1C: {                                                     /* DMULT */
            uint64_t h, l, x = RS, y = RT;
            int neg = ((int64_t)x < 0) ^ ((int64_t)y < 0);
            if ((int64_t)x < 0) x = 0 - x;
            if ((int64_t)y < 0) y = 0 - y;
            mul64(x, y, &h, &l);
            if (neg) { l = ~l + 1; h = ~h + (l == 0); }
            c->hi = h; c->lo = l;
            break;
        }
        case 0x1D: mul64(RS, RT, &c->hi, &c->lo); break;                 /* DMULTU */
        case 0x1E: {                                                     /* DDIV */
            int64_t n = (int64_t)RS, d = (int64_t)RT;
            if (d == 0) { c->lo = n < 0 ? 1 : (uint64_t)-1; c->hi = (uint64_t)n; }
            else if (n == INT64_MIN && d == -1) { c->lo = (uint64_t)n; c->hi = 0; }
            else { c->lo = (uint64_t)(n / d); c->hi = (uint64_t)(n % d); }
            break;
        }
        case 0x1F: {                                                     /* DDIVU */
            uint64_t n = RS, d = RT;
            if (d == 0) { c->lo = (uint64_t)-1; c->hi = n; }
            else { c->lo = n / d; c->hi = n % d; }
            break;
        }
        case 0x20: {                                                     /* ADD */
            int32_t x = (int32_t)RS, y = (int32_t)RT, r = (int32_t)((uint32_t)x + (uint32_t)y);
            if (((x ^ r) & (y ^ r)) < 0) { exception(c, EXC_OV, 0x180); return; }
            RD = SE32(r);
            break;
        }
        case 0x21: RD = SE32((uint32_t)RS + (uint32_t)RT); break;        /* ADDU */
        case 0x22: {                                                     /* SUB */
            int32_t x = (int32_t)RS, y = (int32_t)RT, r = (int32_t)((uint32_t)x - (uint32_t)y);
            if (((x ^ y) & (x ^ r)) < 0) { exception(c, EXC_OV, 0x180); return; }
            RD = SE32(r);
            break;
        }
        case 0x23: RD = SE32((uint32_t)RS - (uint32_t)RT); break;        /* SUBU */
        case 0x24: RD = RS & RT; break;
        case 0x25: RD = RS | RT; break;
        case 0x26: RD = RS ^ RT; break;
        case 0x27: RD = ~(RS | RT); break;
        case 0x2A: RD = (int64_t)RS < (int64_t)RT; break;                /* SLT */
        case 0x2B: RD = RS < RT; break;                                  /* SLTU */
        case 0x2C: {                                                     /* DADD */
            uint64_t r = RS + RT;
            if ((int64_t)((RS ^ r) & (RT ^ r)) < 0) { exception(c, EXC_OV, 0x180); return; }
            RD = r;
            break;
        }
        case 0x2D: RD = RS + RT; break;                                  /* DADDU */
        case 0x2E: {                                                     /* DSUB */
            uint64_t r = RS - RT;
            if ((int64_t)((RS ^ RT) & (RS ^ r)) < 0) { exception(c, EXC_OV, 0x180); return; }
            RD = r;
            break;
        }
        case 0x2F: RD = RS - RT; break;                                  /* DSUBU */
        case 0x30: if ((int64_t)RS >= (int64_t)RT) { exception(c, EXC_TR, 0x180); return; } break;   /* TGE */
        case 0x31: if (RS >= RT) { exception(c, EXC_TR, 0x180); return; } break;                     /* TGEU */
        case 0x32: if ((int64_t)RS < (int64_t)RT) { exception(c, EXC_TR, 0x180); return; } break;    /* TLT */
        case 0x33: if (RS < RT) { exception(c, EXC_TR, 0x180); return; } break;                      /* TLTU */
        case 0x34: if (RS == RT) { exception(c, EXC_TR, 0x180); return; } break;                     /* TEQ */
        case 0x36: if (RS != RT) { exception(c, EXC_TR, 0x180); return; } break;                     /* TNE */
        case 0x38: RD = RT << SA; break;                                 /* DSLL */
        case 0x3A: RD = RT >> SA; break;                                 /* DSRL */
        case 0x3B: RD = (uint64_t)((int64_t)RT >> SA); break;            /* DSRA */
        case 0x3C: RD = RT << (SA + 32); break;                          /* DSLL32 */
        case 0x3E: RD = RT >> (SA + 32); break;                          /* DSRL32 */
        case 0x3F: RD = (uint64_t)((int64_t)RT >> (SA + 32)); break;     /* DSRA32 */
        default: exception(c, EXC_RI, 0x180); return;
        }
        break;
    }
    case 0x01: {                                                         /* REGIMM */
        int rt = (ins >> 16) & 31;
        int64_t s = (int64_t)RS;
        switch (rt) {
        case 0x00: branch(c, s < 0, ins, 0, 0); break;                   /* BLTZ */
        case 0x01: branch(c, s >= 0, ins, 0, 0); break;                  /* BGEZ */
        case 0x02: branch(c, s < 0, ins, 1, 0); break;                   /* BLTZL */
        case 0x03: branch(c, s >= 0, ins, 1, 0); break;                  /* BGEZL */
        case 0x08: if ((uint64_t)s >= SE16(ins)) { exception(c, EXC_TR, 0x180); return; } break;     /* TGEIU... */
        case 0x10: branch(c, s < 0, ins, 0, 1); break;                   /* BLTZAL */
        case 0x11: branch(c, s >= 0, ins, 0, 1); break;                  /* BGEZAL */
        case 0x12: branch(c, s < 0, ins, 1, 1); break;                   /* BLTZALL */
        case 0x13: branch(c, s >= 0, ins, 1, 1); break;                  /* BGEZALL */
        default: break;                                                  /* autres traps : ignores */
        }
        break;
    }
    case 0x02: c->next_pc = ((c->pc + 4) & 0xF0000000u) | ((ins & 0x3FFFFFF) << 2); c->branch_pending = 1; break;  /* J */
    case 0x03: c->r[31] = SE32(c->pc + 8); c->next_pc = ((c->pc + 4) & 0xF0000000u) | ((ins & 0x3FFFFFF) << 2); c->branch_pending = 1; break; /* JAL */
    case 0x04: branch(c, RS == RT, ins, 0, 0); break;                    /* BEQ */
    case 0x05: branch(c, RS != RT, ins, 0, 0); break;                    /* BNE */
    case 0x06: branch(c, (int64_t)RS <= 0, ins, 0, 0); break;            /* BLEZ */
    case 0x07: branch(c, (int64_t)RS > 0, ins, 0, 0); break;             /* BGTZ */
    case 0x08: {                                                         /* ADDI */
        int32_t x = (int32_t)RS, y = IMM, r = (int32_t)((uint32_t)x + (uint32_t)y);
        if (((x ^ r) & (y ^ r)) < 0) { exception(c, EXC_OV, 0x180); return; }
        RT = SE32(r);
        break;
    }
    case 0x09: RT = SE32((uint32_t)RS + (uint32_t)(int32_t)IMM); break;  /* ADDIU */
    case 0x0A: RT = (int64_t)RS < (int64_t)IMM; break;                   /* SLTI */
    case 0x0B: RT = RS < (uint64_t)(int64_t)IMM; break;                  /* SLTIU */
    case 0x0C: RT = RS & (uint16_t)ins; break;                           /* ANDI */
    case 0x0D: RT = RS | (uint16_t)ins; break;                           /* ORI */
    case 0x0E: RT = RS ^ (uint16_t)ins; break;                           /* XORI */
    case 0x0F: RT = SE32((uint32_t)(uint16_t)ins << 16); break;          /* LUI */
    case 0x10: cop0(c, ins); break;
    case 0x11: cop1(c, ins); break;
    case 0x14: branch(c, RS == RT, ins, 1, 0); break;                    /* BEQL */
    case 0x15: branch(c, RS != RT, ins, 1, 0); break;                    /* BNEL */
    case 0x16: branch(c, (int64_t)RS <= 0, ins, 1, 0); break;            /* BLEZL */
    case 0x17: branch(c, (int64_t)RS > 0, ins, 1, 0); break;             /* BGTZL */
    case 0x18: {                                                         /* DADDI */
        uint64_t r = RS + (uint64_t)(int64_t)IMM;
        if ((int64_t)((RS ^ r) & ((uint64_t)(int64_t)IMM ^ r)) < 0) { exception(c, EXC_OV, 0x180); return; }
        RT = r;
        break;
    }
    case 0x19: RT = RS + (uint64_t)(int64_t)IMM; break;                  /* DADDIU */
    case 0x1A: case 0x1B: {                                              /* LDL / LDR */
        uint32_t pa;
        a = VADDR;
        if (vr4300_virt_to_phys(c, a & ~7u, 0, &pa)) return;
        v = phys_read(c, pa, 8);
        {
            unsigned sh = (a & 7) * 8;
            if (op == 0x1A) RT = sh ? ((RT & (((uint64_t)1 << sh) - 1)) | (v << sh)) : v;
            else { unsigned s2 = (7 - (a & 7)) * 8; RT = s2 ? ((RT & ~(~(uint64_t)0 >> s2)) | (v >> s2)) : v; }
        }
        break;
    }
    case 0x20: a = VADDR; if (load(c, a, 1, &v)) return; RT = SE8(v); break;     /* LB */
    case 0x21: a = VADDR; if (load(c, a, 2, &v)) return; RT = SE16(v); break;    /* LH */
    case 0x22: case 0x26: {                                              /* LWL / LWR */
        uint32_t pa, w;
        uint64_t r;
        a = VADDR;
        if (vr4300_virt_to_phys(c, a & ~3u, 0, &pa)) return;
        w = (uint32_t)phys_read(c, pa, 4);
        if (op == 0x22) { unsigned sh = (a & 3) * 8; r = sh ? (((uint32_t)RT & ((1u << sh) - 1)) | (w << sh)) : w; }
        else { unsigned sh = (3 - (a & 3)) * 8; r = sh ? (((uint32_t)RT & ~(0xFFFFFFFFu >> sh)) | (w >> sh)) : w; }
        RT = SE32(r);
        break;
    }
    case 0x23: a = VADDR; if (load(c, a, 4, &v)) return; RT = SE32(v); break;    /* LW */
    case 0x24: a = VADDR; if (load(c, a, 1, &v)) return; RT = (uint8_t)v; break; /* LBU */
    case 0x25: a = VADDR; if (load(c, a, 2, &v)) return; RT = (uint16_t)v; break;/* LHU */
    case 0x27: a = VADDR; if (load(c, a, 4, &v)) return; RT = (uint32_t)v; break;/* LWU */
    case 0x28: a = VADDR; if (store(c, a, 1, RT)) return; break;         /* SB */
    case 0x29: a = VADDR; if (store(c, a, 2, RT)) return; break;         /* SH */
    case 0x2A: case 0x2E: {                                              /* SWL / SWR */
        uint32_t pa, w, r = (uint32_t)RT;
        a = VADDR;
        if (vr4300_virt_to_phys(c, a & ~3u, 1, &pa)) return;
        w = (uint32_t)phys_read(c, pa, 4);
        if (op == 0x2A) { unsigned sh = (a & 3) * 8; w = sh ? ((w & ~(0xFFFFFFFFu >> sh)) | (r >> sh)) : r; }
        else { unsigned sh = (3 - (a & 3)) * 8; w = sh ? ((w & ((1u << sh) - 1)) | (r << sh)) : r; }
        phys_write(c, pa, 4, w);
        break;
    }
    case 0x2B: a = VADDR; if (store(c, a, 4, RT)) return; break;         /* SW */
    case 0x2C: case 0x2D: {                                              /* SDL / SDR */
        uint32_t pa;
        uint64_t w, r = RT;
        a = VADDR;
        if (vr4300_virt_to_phys(c, a & ~7u, 1, &pa)) return;
        w = phys_read(c, pa, 8);
        if (op == 0x2C) { unsigned sh = (a & 7) * 8; w = sh ? ((w & ~(~(uint64_t)0 >> sh)) | (r >> sh)) : r; }
        else { unsigned sh = (7 - (a & 7)) * 8; w = sh ? ((w & (((uint64_t)1 << sh) - 1)) | (r << sh)) : r; }
        phys_write(c, pa, 8, w);
        break;
    }
    case 0x2F: break;                                                    /* CACHE */
    case 0x30: a = VADDR; if (load(c, a, 4, &v)) return; RT = SE32(v); c->llbit = 1; break;     /* LL */
    case 0x31: a = VADDR; if (load(c, a, 4, &v)) return; fs_set_bits(c, (ins >> 16) & 31, (uint32_t)v); break;  /* LWC1 */
    case 0x34: a = VADDR; if (load(c, a, 8, &v)) return; RT = v; c->llbit = 1; break;           /* LLD */
    case 0x35: a = VADDR; if (load(c, a, 8, &v)) return; fd_set_bits(c, (ins >> 16) & 31, v); break;            /* LDC1 */
    case 0x37: a = VADDR; if (load(c, a, 8, &v)) return; RT = v; break;                         /* LD */
    case 0x38: a = VADDR; if (c->llbit) { if (store(c, a, 4, RT)) return; } RT = (uint64_t)c->llbit; break;     /* SC */
    case 0x39: {                                                                                 /* SWC1 */
        int f = (ins >> 16) & 31;
        uint32_t bits = (c->cp0[CP0_STATUS] & ST_FR) ? (uint32_t)c->fpr[f] : ((f & 1) ? (uint32_t)(c->fpr[f & ~1] >> 32) : (uint32_t)c->fpr[f]);
        a = VADDR;
        if (store(c, a, 4, bits)) return;
        break;
    }
    case 0x3C: a = VADDR; if (c->llbit) { if (store(c, a, 8, RT)) return; } RT = (uint64_t)c->llbit; break;     /* SCD */
    case 0x3D: a = VADDR; if (store(c, a, 8, fd_bits(c, (ins >> 16) & 31))) return; break;     /* SDC1 */
    case 0x3F: a = VADDR; if (store(c, a, 8, RT)) return; break;                                /* SD */
    default: exception(c, EXC_RI, 0x180); return;
    }
}

/* ------------------------------------------------------------------ boucles d'attente
 * Une petite boucle sans ecriture memoire, dont chaque registre lu est soit
 * invariant, soit recalcule plus tot dans la meme iteration, refait exactement
 * le meme travail tant que rien d'exterieur ne change. Or l'exterieur (VI, AI,
 * interruptions...) ne change qu'entre deux tranches de vr4300_run : on peut
 * donc sauter directement a la fin de la tranche. */

static int pure_op(uint32_t ins, uint32_t *rd_mask, uint32_t *rs_mask)
{
    int op = (int)(ins >> 26), rs = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31), rd = (int)((ins >> 11) & 31);
    *rd_mask = *rs_mask = 0;
    switch (op) {
    case 0x00:
        switch (ins & 63) {
        case 0x00: case 0x02: case 0x03:                          /* SLL SRL SRA */
            *rs_mask = 1u << rt; *rd_mask = 1u << rd; return 1;
        case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x2A: case 0x2B: case 0x2D: case 0x2F:
            *rs_mask = 1u << rs | 1u << rt; *rd_mask = 1u << rd; return 1;
        }
        return 0;
    case 0x09: case 0x19: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:      /* ADDIU... ORI */
        *rs_mask = 1u << rs; *rd_mask = 1u << rt; return 1;
    case 0x0F: *rd_mask = 1u << rt; return 1;                     /* LUI */
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37:      /* chargements */
        *rs_mask = 1u << rs; *rd_mask = 1u << rt; return 1;
    }
    return 0;
}

static int final_branch(uint32_t ins, uint32_t *rs_mask)
{
    int op = (int)(ins >> 26), rs = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31);
    *rs_mask = 0;
    switch (op) {
    case 0x02: return 1;                                          /* J */
    case 0x04: case 0x05: case 0x14: case 0x15: *rs_mask = 1u << rs | 1u << rt; return 1;
    case 0x06: case 0x07: case 0x16: case 0x17: *rs_mask = 1u << rs; return 1;
    case 0x01: if (rt == 0 || rt == 1 || rt == 2 || rt == 3) { *rs_mask = 1u << rs; return 1; } return 0;
    }
    return 0;
}

static int fetch_word(VR4300 *c, uint32_t va, uint32_t *w)
{
    uint32_t pa;
    if ((va & 0xC0000000u) != 0x80000000u) return -1;
    pa = va & 0x1FFFFFFF;
    if (pa + 4 > c->rdram_size) return -1;
    *w = ld32(c->rdram + pa);
    return 0;
}

/* boucle [target .. branch_pc + 4] (creneau inclus) */
static int is_idle_loop(VR4300 *c, uint32_t target, uint32_t branch_pc)
{
    uint32_t n = (branch_pc - target) / 4 + 2, i, w[10], sig = 2166136261u, written = 0, defined = 0, rdm, rsm;
    unsigned slot;
    if (target > branch_pc || n > 10) return 0;
    for (i = 0; i < n; i++) {
        if (fetch_word(c, target + i * 4, &w[i])) return 0;
        sig = (sig ^ w[i]) * 16777619u;
    }
    slot = (branch_pc >> 2) & 63;
    if (c->idle_cache[slot].pc == branch_pc && c->idle_cache[slot].sig == sig) return c->idle_cache[slot].verdict;
    /* registres ecrits dans la boucle */
    for (i = 0; i < n; i++) {
        if (i == n - 2) { if (!final_branch(w[i], &rsm)) goto no; continue; }
        if (!pure_op(w[i], &rdm, &rsm)) goto no;
        written |= rdm;
    }
    written &= ~1u;
    /* chaque lecture d'un registre modifie doit suivre son calcul dans l'iteration */
    for (i = 0; i < n; i++) {
        if (i == n - 2) final_branch(w[i], &rsm), rdm = 0;
        else pure_op(w[i], &rdm, &rsm);
        if (rsm & written & ~defined) goto no;
        defined |= rdm;
    }
    c->idle_cache[slot].pc = branch_pc; c->idle_cache[slot].sig = sig; c->idle_cache[slot].verdict = 1;
    return 1;
no:
    c->idle_cache[slot].pc = branch_pc; c->idle_cache[slot].sig = sig; c->idle_cache[slot].verdict = 0;
    return 0;
}

/* fait avancer le temps de n cycles (Count/Compare compris) */
static void advance(VR4300 *c, uint64_t n)
{
    uint64_t halves = c->count_half + n, k = halves / 2;
    uint32_t cnt = (uint32_t)c->cp0[CP0_COUNT], cmp = (uint32_t)c->cp0[CP0_COMPARE];
    c->count_half = (unsigned)(halves & 1);
    if (k) {
        uint32_t dist = cmp - cnt;                    /* increments avant d'atteindre Compare */
        if (dist != 0 && (uint64_t)dist <= k) c->cp0[CP0_CAUSE] |= 0x8000;
        c->cp0[CP0_COUNT] = (uint32_t)(cnt + (uint32_t)k);
    }
    c->cycles += n;
}

uint64_t vr4300_run(VR4300 *c, uint64_t cycles)
{
    uint64_t start = c->cycles, end = start + cycles;
    const unsigned cpi = c->cpi ? c->cpi : 1;
    while (c->cycles < end) {
        uint32_t ins, pa, this_pc;
        uint64_t exc;

        if (!c->in_delay && (c->cp0[CP0_CAUSE] & 0xFF00) && check_interrupts(c)) { c->cycles += 2; continue; }

        this_pc = c->pc;
        if ((this_pc & 0xC0000000u) == 0x80000000u && (pa = this_pc & 0x1FFFFFFF) + 4 <= c->rdram_size)
            ins = ld32(c->rdram + pa);
        else {
            if ((this_pc & 0xC0000000u) == 0x80000000u) pa = this_pc & 0x1FFFFFFF;
            else if (this_pc & 3) { address_error(c, this_pc, 0); continue; }
            else if (vr4300_virt_to_phys(c, this_pc, 0, &pa)) continue;
            ins = (uint32_t)phys_read(c, pa, 4);
        }

        exc = c->exceptions;
        c->branch_pending = 0;
        execute(c, ins);
        c->r[0] = 0;
        c->instructions++;

        /* temps : Count = +1 tous les 2 cycles */
        c->cycles += cpi;
        c->count_half += cpi;
        while (c->count_half >= 2) {
            c->count_half -= 2;
            c->cp0[CP0_COUNT] = (uint32_t)(c->cp0[CP0_COUNT] + 1);
            if ((uint32_t)c->cp0[CP0_COUNT] == (uint32_t)c->cp0[CP0_COMPARE]) c->cp0[CP0_CAUSE] |= 0x8000;
        }

        if (c->exceptions != exc) { c->in_delay = 0; continue; }      /* pc = vecteur d'exception */

        if (c->in_delay) {                         /* on vient d'executer le creneau de retard */
            c->pc = c->delay_target;
            c->in_delay = 0;
            if (c->idle_skip && c->delay_target <= this_pc && c->cycles < end &&
                is_idle_loop(c, c->delay_target, this_pc - 4)) {
                c->idle_skips++;
                c->idle_cycles += end - c->cycles;
                advance(c, end - c->cycles);
            }
        } else if (c->branch_pending == 1) {       /* branchement : executer d'abord le creneau */
            c->delay_target = c->next_pc;
            c->pc = this_pc + 4;
            c->in_delay = 1;
        } else if (c->branch_pending == 3) {       /* "likely" non pris : creneau annule */
            c->pc = this_pc + 8;
        } else if (c->branch_pending == 4) {       /* ERET */
            c->pc = c->next_pc;
        } else {
            c->pc = this_pc + 4;
        }
    }
    return c->cycles - start;
}

/* ------------------------------------------------------------------ services du recompilateur */

#ifdef JIT_STATS
uint64_t jit_stat_op[64], jit_stat_cop1[64];
#endif
int vr4300_exec1(VR4300 *c, uint32_t ins)
{
    uint64_t e = c->exceptions;
#ifdef JIT_STATS
    jit_stat_op[ins >> 26]++;
    if ((ins >> 26) == 17) jit_stat_cop1[(ins >> 21) & 31]++;
#endif
    c->branch_pending = 0;
    execute(c, ins);
    c->r[0] = 0;
    if (c->cp0[CP0_CAUSE] & c->cp0[CP0_STATUS] & 0xFF00) c->jit_budget = 0;   /* interruption : retour au C */
    return c->exceptions != e;
}

int vr4300_jit_load(VR4300 *c, uint32_t va, uint32_t kind)
{
    int rt = (int)(kind & 31), size = (int)((kind >> 8) & 15), sgn = (int)((kind >> 12) & 1);
    uint64_t v;
    if (load(c, va, size, &v)) return 1;
    if (size == 1) v &= 0xFF; else if (size == 2) v &= 0xFFFF; else if (size == 4) v &= 0xFFFFFFFFu;
    if (sgn) {
        if (size == 1) v = (uint64_t)(int64_t)(int8_t)v;
        else if (size == 2) v = (uint64_t)(int64_t)(int16_t)v;
        else if (size == 4) v = (uint64_t)(int64_t)(int32_t)v;
    }
    if (rt) c->r[rt] = v;
    if (c->cp0[CP0_CAUSE] & c->cp0[CP0_STATUS] & 0xFF00) c->jit_budget = 0;
    return 0;
}

int vr4300_jit_store(VR4300 *c, uint32_t va, uint32_t kind)
{
    int rt = (int)(kind & 31), size = (int)((kind >> 8) & 15);
    uint64_t pages = c->exceptions;
    store(c, va, size, c->r[rt]);
    if (c->cp0[CP0_CAUSE] & c->cp0[CP0_STATUS] & 0xFF00) c->jit_budget = 0;
    return c->exceptions != pages;
}

int vr4300_check_int(VR4300 *c) { return check_interrupts(c); }
void vr4300_advance(VR4300 *c, uint64_t n) { advance(c, n); }
int vr4300_idle_loop(VR4300 *c, uint32_t target, uint32_t branch_pc) { return is_idle_loop(c, target, branch_pc); }
