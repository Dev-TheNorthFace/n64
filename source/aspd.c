/* aspd.c : microcode audio HLE (ABI1 et NEAD). */
#include "aspd.h"
#include <string.h>

/* ------------------------------------------------------------ acces memoire */

static uint8_t *R;          /* RDRAM de l'appel en cours */
static uint32_t RS;

static inline int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static inline uint8_t d8(uint32_t a) { a &= 0xFFFFFF; return a < RS ? R[a] : 0; }
static inline int16_t d16(uint32_t a) { return (int16_t)(d8(a) << 8 | d8(a + 1)); }
static inline int32_t d32(uint32_t a) { return (int32_t)((uint32_t)(uint16_t)d16(a) << 16 | (uint16_t)d16(a + 2)); }
static inline void w8(uint32_t a, uint8_t v) { a &= 0xFFFFFF; if (a < RS) R[a] = v; }
static inline void w16d(uint32_t a, int16_t v) { w8(a, (uint8_t)((uint16_t)v >> 8)); w8(a + 1, (uint8_t)v); }
static inline void w32d(uint32_t a, int32_t v) { w16d(a, (int16_t)((uint32_t)v >> 16)); w16d(a + 2, (int16_t)v); }

/* echantillon 16 bits dans la DMEM (adresse en octets) */
static inline int16_t S(const Aspd *a, uint32_t o) { return (int16_t)(a->buf[o & 0xFFF] << 8 | a->buf[(o + 1) & 0xFFF]); }
static inline void W(Aspd *a, uint32_t o, int16_t v) { a->buf[o & 0xFFF] = (uint8_t)((uint16_t)v >> 8); a->buf[(o + 1) & 0xFFF] = (uint8_t)v; }

static inline uint32_t align(uint32_t v, uint32_t m) { return (v + m - 1) & ~(m - 1); }

/* ------------------------------------------------------------ table du reechantillonneur
 * 64 phases x 4 coefficients (Q15), filtre d'interpolation adouci proche de
 * celui du microcode (centre sur le 2e echantillon). */
static int16_t lut[64][4];
static int lut_ok;

static void make_lut(void)
{
    int p, k;
    for (p = 0; p < 64; p++) {
        double f = p / 64.0, f2 = f * f, f3 = f2 * f, w[4], b[4], s = 0;
        /* Catmull-Rom */
        w[0] = -0.5 * f3 + f2 - 0.5 * f;
        w[1] = 1.5 * f3 - 2.5 * f2 + 1;
        w[2] = -1.5 * f3 + 2 * f2 + 0.5 * f;
        w[3] = 0.5 * f3 - 0.5 * f2;
        /* B-spline cubique (plus doux) */
        b[0] = (1 - f) * (1 - f) * (1 - f) / 6;
        b[1] = (3 * f3 - 6 * f2 + 4) / 6;
        b[2] = (-3 * f3 + 3 * f2 + 3 * f + 1) / 6;
        b[3] = f3 / 6;
        for (k = 0; k < 4; k++) { w[k] = 0.45 * w[k] + 0.55 * b[k]; s += w[k]; }
        for (k = 0; k < 4; k++) lut[p][k] = (int16_t)(w[k] / s * 32767.0 + (w[k] >= 0 ? 0.5 : -0.5));
    }
    lut_ok = 1;
}

/* ------------------------------------------------------------ operations communes */

static void op_clear(Aspd *a, uint32_t dmem, uint32_t count) { while (count--) a->buf[dmem++ & 0xFFF] = 0; }

static void op_load(Aspd *a, uint32_t dmem, uint32_t addr, uint32_t count)
{
    uint32_t i;
    dmem &= ~3u; addr &= ~7u; count = align(count, 8);
    for (i = 0; i < count; i++) a->buf[(dmem + i) & 0xFFF] = d8(addr + i);
}

static void op_save(Aspd *a, uint32_t dmem, uint32_t addr, uint32_t count)
{
    uint32_t i;
    dmem &= ~3u; addr &= ~7u; count = align(count, 8);
    for (i = 0; i < count; i++) w8(addr + i, a->buf[(dmem + i) & 0xFFF]);
}

static void op_move(Aspd *a, uint32_t o, uint32_t i, uint32_t count) { while (count--) a->buf[o++ & 0xFFF] = a->buf[i++ & 0xFFF]; }

static void op_mix(Aspd *a, uint32_t o, uint32_t i, uint32_t count, int16_t gain)
{
    uint32_t n = count >> 1, k;
    for (k = 0; k < n; k++) W(a, o + k * 2, clamp16(S(a, o + k * 2) + ((S(a, i + k * 2) * gain) >> 15)));
}

static void op_interleave(Aspd *a, uint32_t o, uint32_t l, uint32_t r, uint32_t count)
{
    uint32_t n = count >> 1, k;
    int16_t tmp[0x800];
    if (n > 0x400) n = 0x400;
    for (k = 0; k < n; k++) { tmp[k * 2] = S(a, l + k * 2); tmp[k * 2 + 1] = S(a, r + k * 2); }
    for (k = 0; k < n * 2; k++) W(a, o + k * 2, tmp[k]);
}

static void op_resample(Aspd *a, int init, uint32_t o, uint32_t i, uint32_t count, uint32_t pitch, uint32_t addr)
{
    uint32_t ipos = (i >> 1) - 4, opos = o >> 1, n = count >> 1, accu, k;
    if (!lut_ok) make_lut();
    if (init) { for (k = 0; k < 4; k++) W(a, (ipos + k) * 2, 0); accu = 0; }
    else { for (k = 0; k < 4; k++) W(a, (ipos + k) * 2, d16(addr + k * 2)); accu = (uint16_t)d16(addr + 8); }
    while (n--) {
        const int16_t *t = lut[(accu >> 10) & 63];
        int32_t s = S(a, ipos * 2) * t[0] + S(a, (ipos + 1) * 2) * t[1] + S(a, (ipos + 2) * 2) * t[2] + S(a, (ipos + 3) * 2) * t[3];
        W(a, (opos++) * 2, clamp16(s >> 15));
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xFFFF;
    }
    for (k = 0; k < 4; k++) w16d(addr + k * 2, S(a, (ipos + k) * 2));
    w16d(addr + 8, (int16_t)accu);
}

static void op_resample_zoh(Aspd *a, uint32_t o, uint32_t i, uint32_t count, uint32_t pitch, uint32_t accu)
{
    uint32_t ipos = i >> 1, opos = o >> 1, n = count >> 1;
    while (n--) {
        W(a, (opos++) * 2, S(a, ipos * 2));
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xFFFF;
    }
}

static void op_adpcm(Aspd *a, int init, int loop, int two_bit, uint32_t o, uint32_t i, uint32_t count, uint32_t loop_addr, uint32_t last_addr)
{
    int16_t hist[16];
    int k;
    if (init) memset(hist, 0, sizeof hist);
    else for (k = 0; k < 16; k++) hist[k] = d16((loop ? loop_addr : last_addr) + (uint32_t)k * 2);
    for (k = 0; k < 16; k++) W(a, o + (uint32_t)k * 2, hist[k]);
    o += 32;
    while (count >= 32) {
        uint8_t code = a->buf[i++ & 0xFFF];
        int scale = code >> 4, h, j;
        const int16_t *book = a->table + (code & 15) * 16;
        int32_t res[16];
        for (k = 0; k < 16; k++) {
            int v;
            if (two_bit) {
                uint8_t b = a->buf[(i + (uint32_t)(k >> 2)) & 0xFFF];
                v = (b >> (6 - (k & 3) * 2)) & 3;
                v = (int16_t)(uint16_t)(v << 14) >> (scale < 14 ? 14 - scale : 0);
            } else {
                uint8_t b = a->buf[(i + (uint32_t)(k >> 1)) & 0xFFF];
                v = (k & 1) ? (b & 15) : (b >> 4);
                v = (int16_t)(uint16_t)(v << 12) >> (scale < 12 ? 12 - scale : 0);
            }
            res[k] = v;
        }
        i += two_bit ? 4 : 8;
        for (h = 0; h < 2; h++) {
            int32_t p2 = hist[14], p1 = hist[15];
            int16_t outv[8];
            for (j = 0; j < 8; j++) {
                int64_t tot = (int64_t)book[j] * p2 + (int64_t)book[8 + j] * p1 + (int64_t)res[h * 8 + j] * 2048;
                for (k = 0; k < j; k++) tot += (int64_t)book[8 + j - 1 - k] * res[h * 8 + k];
                tot >>= 11;
                outv[j] = clamp16(tot < -40000 ? -40000 : tot > 40000 ? 40000 : (int32_t)tot);
            }
            /* l'historique devient ce demi-bloc (les 2 derniers servent a la prediction) */
            for (j = 0; j < 8; j++) { hist[j] = hist[8 + j]; hist[8 + j] = outv[j]; }
            for (j = 0; j < 8; j++) W(a, o + (uint32_t)j * 2, outv[j]);
            o += 16;
        }
        count -= 32;
    }
    for (k = 0; k < 16; k++) w16d(last_addr + (uint32_t)k * 2, hist[k]);
}

static void op_polef(Aspd *a, int init, uint32_t o, uint32_t i, uint32_t count, int16_t gain, uint32_t addr)
{
    int16_t h1[8], h2[8], h2b[8], l1, l2, frame[8], outv[8] = { 0 };
    int k, j;
    count = align(count, 16);
    for (k = 0; k < 8; k++) { h1[k] = a->table[k]; h2b[k] = a->table[8 + k]; h2[k] = (int16_t)((a->table[8 + k] * gain) >> 14); }
    if (init) l1 = l2 = 0;
    else { l1 = d16(addr + 4); l2 = d16(addr + 6); }
    while (count) {
        for (k = 0; k < 8; k++) frame[k] = S(a, i + (uint32_t)k * 2);
        for (k = 0; k < 8; k++) {
            int32_t acc = frame[k] * gain + h1[k] * l1 + h2b[k] * l2;
            for (j = 0; j < k; j++) acc += h2[j] * frame[k - 1 - j];
            outv[k] = clamp16(acc >> 14);
            W(a, o + (uint32_t)k * 2, outv[k]);
        }
        l1 = outv[6]; l2 = outv[7];
        i += 16; o += 16; count -= 16;
    }
    for (k = 0; k < 4; k++) w16d(addr + (uint32_t)k * 2, outv[4 + k]);
}

/* enveloppe exponentielle de l'ABI 1 */
typedef struct { int32_t value, target, step, rate, seq; } Ramp;

static int16_t ramp_next(Ramp *r)
{
    int reached;
    r->value = (int32_t)((uint32_t)r->value + (uint32_t)r->step);
    reached = r->step <= 0 ? r->value <= r->target : r->value >= r->target;
    if (reached) { r->value = r->target; r->step = 0; }
    return (int16_t)(r->value >> 16);
}

static void op_envmix_exp(Aspd *a, int init, int aux, uint32_t addr)
{
    Ramp rp[2];
    int16_t dry, wet;
    uint32_t count = a->count, ptr = 0;
    int k, x;
    if (init) {
        dry = a->dry; wet = a->wet;
        for (k = 0; k < 2; k++) {
            rp[k].value = (int32_t)((uint32_t)(uint16_t)a->vol[k] << 16);
            rp[k].target = (int32_t)((uint32_t)(uint16_t)a->target[k] << 16);
            rp[k].rate = a->rate[k];
            rp[k].seq = (int32_t)(uint32_t)((int64_t)a->vol[k] * a->rate[k]);
        }
    } else {
        wet = d16(addr); dry = d16(addr + 4);
        for (k = 0; k < 2; k++) {
            rp[k].target = d32(addr + 8 + (uint32_t)k * 4);
            rp[k].rate = d32(addr + 16 + (uint32_t)k * 4);
            rp[k].seq = d32(addr + 24 + (uint32_t)k * 4);
            rp[k].value = d32(addr + 32 + (uint32_t)k * 4);
        }
    }
    for (k = 0; k < 2; k++) rp[k].step = (int32_t)((uint32_t)rp[k].target - (uint32_t)rp[k].value);
    while (count >= 16) {
        for (k = 0; k < 2; k++)
            if (rp[k].step != 0) {
                rp[k].seq = (int32_t)(uint32_t)(((int64_t)rp[k].seq * rp[k].rate) >> 16);
                rp[k].step = (int32_t)(((int64_t)rp[k].seq - rp[k].value) >> 3);
            }
        for (x = 0; x < 8; x++, ptr += 2) {
            int16_t lv = ramp_next(&rp[0]), rv = ramp_next(&rp[1]);
            int16_t gld = clamp16((lv * dry + 0x4000) >> 15), grd = clamp16((rv * dry + 0x4000) >> 15);
            int16_t s = S(a, a->in + ptr);
            W(a, a->out + ptr, clamp16(S(a, a->out + ptr) + ((s * gld) >> 15)));
            W(a, a->dry_right + ptr, clamp16(S(a, a->dry_right + ptr) + ((s * grd) >> 15)));
            if (aux) {
                int16_t glw = clamp16((lv * wet + 0x4000) >> 15), grw = clamp16((rv * wet + 0x4000) >> 15);
                W(a, a->wet_left + ptr, clamp16(S(a, a->wet_left + ptr) + ((s * glw) >> 15)));
                W(a, a->wet_right + ptr, clamp16(S(a, a->wet_right + ptr) + ((s * grw) >> 15)));
            }
        }
        count -= 16;
    }
    w16d(addr, wet); w16d(addr + 4, dry);
    for (k = 0; k < 2; k++) {
        w32d(addr + 8 + (uint32_t)k * 4, rp[k].target);
        w32d(addr + 16 + (uint32_t)k * 4, rp[k].rate);
        w32d(addr + 24 + (uint32_t)k * 4, rp[k].seq);
        w32d(addr + 32 + (uint32_t)k * 4, rp[k].value);
    }
}

/* enveloppe lineaire NEAD */
static void op_envmix_nead(Aspd *a, int swap, uint32_t dl, uint32_t dr, uint32_t wl, uint32_t wr,
                           uint32_t in, uint32_t count, const int16_t xors[4])
{
    uint16_t *env = a->env_values;
    int i;
    if (swap) { uint32_t t = wl; wl = wr; wr = t; }
    count = align(count, 8);
    while (count) {
        for (i = 0; i < 8; i++) {
            uint32_t k = (uint32_t)i * 2;
            int32_t s = S(a, in + k);
            int16_t l = (int16_t)((int16_t)((s * env[0]) >> 16) ^ xors[0]);
            int16_t r = (int16_t)((int16_t)((s * env[1]) >> 16) ^ xors[1]);
            int16_t l2 = (int16_t)((int16_t)((l * env[2]) >> 16) ^ xors[2]);
            int16_t r2 = (int16_t)((int16_t)((r * env[2]) >> 16) ^ xors[3]);
            W(a, dl + k, clamp16(S(a, dl + k) + l));
            W(a, dr + k, clamp16(S(a, dr + k) + r));
            W(a, wl + k, clamp16(S(a, wl + k) + l2));
            W(a, wr + k, clamp16(S(a, wr + k) + r2));
        }
        for (i = 0; i < 3; i++) env[i] = (uint16_t)(env[i] + a->env_steps[i]);
        in += 16; dl += 16; dr += 16; wl += 16; wr += 16;
        count -= 8;
    }
}

static void op_filter(Aspd *a, uint32_t dmem, uint32_t count, uint32_t addr)
{
    int16_t lut6[8], Wv[16], outb[0x3C0];
    uint32_t x, n = 0, cur = dmem;
    int k, j;
    for (k = 0; k < 8; k++) {
        int16_t v = (int16_t)((d16(a->filter_lut[0] + (uint32_t)k * 2) + d16(a->filter_lut[1] + (uint32_t)k * 2)) >> 1);
        lut6[k] = v;
        w16d(a->filter_lut[0] + (uint32_t)k * 2, v);
        w16d(a->filter_lut[1] + (uint32_t)k * 2, v);
    }
    for (k = 0; k < 8; k++) Wv[k] = d16(addr + (uint32_t)k * 2);
    for (x = 0; x < count && n + 8 <= 0x3C0; x += 16) {
        for (k = 0; k < 8; k++) Wv[8 + k] = S(a, cur + (uint32_t)k * 2);
        for (k = 0; k < 8; k++) {
            int32_t v = 0;
            for (j = 0; j < 8; j++) v += Wv[k + 1 + j] * lut6[7 - j];
            outb[n++] = (int16_t)((v + 0x4000) >> 15);
        }
        for (k = 0; k < 8; k++) Wv[k] = Wv[8 + k];
        cur += 16;
    }
    if (n) for (k = 0; k < 8; k++) w16d(addr + (uint32_t)k * 2, Wv[k]);
    for (x = 0; x < n && x * 2 < count; x++) W(a, dmem + x * 2, outb[x]);
}

/* ------------------------------------------------------------ ABI 1 */

#define A_INIT 0x01
#define A_LOOP 0x02
#define A_LEFT 0x02
#define A_VOL  0x04
#define A_AUX  0x08
#define BASE   0x5C0

static uint32_t seg_addr(const Aspd *a, uint32_t so) { return (a->seg[(so >> 24) & 15] + (so & 0xFFFFFF)) & 0xFFFFFF; }

static void abi1(Aspd *a, uint32_t w1, uint32_t w2)
{
    uint32_t op = (w1 >> 24) & 0x1F, flags = (w1 >> 16) & 0xFF, lo = w1 & 0xFFFF;
    switch (op) {
    case 0: break;
    case 1: op_adpcm(a, flags & A_INIT, flags & A_LOOP, 0, a->out, a->in, align(a->count, 32), a->loop, seg_addr(a, w2)); break;
    case 2: if (w2 & 0xFFF) op_clear(a, lo + BASE, align(w2 & 0xFFF, 16)); break;
    case 3: op_envmix_exp(a, flags & A_INIT, flags & A_AUX, seg_addr(a, w2)); break;
    case 4: if (a->count) op_load(a, a->in, seg_addr(a, w2), a->count); break;
    case 5: op_resample(a, flags & A_INIT, a->out, a->in, align(a->count, 16), lo << 1, seg_addr(a, w2)); break;
    case 6: if (a->count) op_save(a, a->out, seg_addr(a, w2), a->count); break;
    case 7: a->seg[(w2 >> 24) & 15] = w2 & 0xFFFFFF; break;
    case 8:
        if (flags & A_AUX) { a->dry_right = (uint16_t)(lo + BASE); a->wet_left = (uint16_t)((w2 >> 16) + BASE); a->wet_right = (uint16_t)((w2 & 0xFFFF) + BASE); }
        else { a->in = (uint16_t)(lo + BASE); a->out = (uint16_t)((w2 >> 16) + BASE); a->count = (uint16_t)w2; }
        break;
    case 9:
        if (flags & A_AUX) { a->dry = (int16_t)lo; a->wet = (int16_t)w2; }
        else {
            int lr = (flags & A_LEFT) ? 0 : 1;
            if (flags & A_VOL) a->vol[lr] = (int16_t)lo;
            else { a->target[lr] = (int16_t)lo; a->rate[lr] = (int32_t)w2; }
        }
        break;
    case 10: if (w2 & 0xFFFF) op_move(a, (w2 >> 16) + BASE, lo + BASE, align(w2 & 0xFFFF, 16)); break;
    case 11: {
        uint32_t n = align(lo, 8) >> 1, k, addr = seg_addr(a, w2);
        if (n > 256) n = 256;
        for (k = 0; k < n; k++) a->table[k] = d16(addr + k * 2);
        break;
    }
    case 12: if (a->count) op_mix(a, (w2 & 0xFFFF) + BASE, (w2 >> 16) + BASE, align(a->count, 32), (int16_t)lo); break;
    case 13: if (a->count) op_interleave(a, a->out, (w2 >> 16) + BASE, (w2 & 0xFFFF) + BASE, align(a->count, 16)); break;
    case 14: if (a->count) op_polef(a, flags & A_INIT, a->out, a->in, align(a->count, 16), (int16_t)lo, seg_addr(a, w2)); break;
    case 15: a->loop = seg_addr(a, w2); break;
    default: a->unknown++; break;
    }
}

/* ------------------------------------------------------------ NEAD (ABI 2) */

static void nead(Aspd *a, int mk, uint32_t w1, uint32_t w2)
{
    uint32_t op = (w1 >> 24) & 0x1F, flags = (w1 >> 16) & 0xFF, lo = w1 & 0xFFFF;
    uint32_t a24 = w2 & 0xFFFFFF, hi2 = w2 >> 16, lo2 = w2 & 0xFFFF;
    switch (op) {
    case 0: if (!mk) a->unknown++; break;
    case 1: op_adpcm(a, flags & 1, flags & 2, flags & 4, a->out, a->in, (a->count + 0x1F) & ~0x1Fu, a->loop, a24); break;
    case 2: if (w2 & 0xFFF) op_clear(a, lo, w2 & 0xFFF); break;
    case 3: if (!mk) a->unknown++; break;
    case 4: if (!mk) { uint32_t n = ((w1 >> 12) & 0xFF0) >> 1, k; for (k = 0; k < n; k++) W(a, lo2 + k * 2, clamp16(S(a, lo2 + k * 2) + S(a, hi2 + k * 2))); } break;
    case 5: op_resample(a, flags & 1, a->out, a->in, (a->count + 0xF) & ~0xFu, lo << 1, a24); break;
    case 6: if (!mk) op_resample_zoh(a, a->out, a->in, a->count, lo << 1, lo2); break;
    case 7:
        if (mk) break;
        if (flags > 1) { a->filter_count = lo; a->filter_lut[0] = a24; }
        else { a->filter_lut[1] = a24 + 0x10; op_filter(a, lo, a->filter_count, a24); }
        break;
    case 8: a->in = (uint16_t)lo; a->out = (uint16_t)hi2; a->count = (uint16_t)lo2; break;
    case 9:
        if (!mk) {
            uint8_t tmp[128];
            uint32_t n = (w1 >> 16) & 0xFF, k, o = hi2;
            for (k = 0; k < 128; k++) tmp[k] = a->buf[(lo + k) & 0xFFF];
            while (n--) { for (k = 0; k < 128; k++) a->buf[(o + k) & 0xFFF] = tmp[k]; o = (o + 128) & 0xFFFF; }
        }
        break;
    case 10: if (lo2) op_move(a, hi2, lo, (lo2 + 3) & ~3u); break;
    case 11: { uint32_t n = lo >> 1, k; if (n > 256) n = 256; for (k = 0; k < n; k++) a->table[k] = d16(a24 + k * 2); break; }
    case 12: op_mix(a, lo2, hi2, (w1 >> 12) & 0xFF0, (int16_t)lo); break;
    case 13:
        if (mk) { if (a->count) op_interleave(a, a->out, hi2, lo2, a->count); }
        else op_interleave(a, lo, hi2, lo2, (w1 >> 12) & 0xFF0);
        break;
    case 14:
        if (mk) { if (a->count) op_polef(a, flags & A_INIT, a->out, a->in, a->count, (int16_t)lo, a24); }
        else {                                       /* HILOGAIN */
            int8_t gain = (int8_t)(w1 >> 16);
            uint32_t n = (w1 & 0xFFF) >> 1, k;
            for (k = 0; k < n; k++) W(a, hi2 + k * 2, clamp16((S(a, hi2 + k * 2) * gain) >> 4));
        }
        break;
    case 15: a->loop = a24; break;
    case 16: {                                       /* copie de blocs */
        uint32_t blocks = (w1 >> 16) & 0xFF, bs = lo2, i = lo, o = hi2;
        if (!blocks) blocks = 1;
        while (blocks--) {
            int32_t left = (int32_t)bs;
            do { uint32_t k; for (k = 0; k < 32; k++) a->buf[(o + k) & 0xFFF] = a->buf[(i + k) & 0xFFF]; left -= 32; i += 32; o += 32; } while (left > 0);
        }
        break;
    }
    case 17: { uint32_t n = lo, i = hi2, o = lo2; while (n--) { W(a, o, S(a, i)); o += 2; i += 4; } break; }
    case 18:
        a->env_values[2] = (uint16_t)((w1 >> 8) & 0xFF00);
        a->env_steps[2] = mk ? 0 : (uint16_t)lo;
        a->env_steps[0] = (uint16_t)hi2; a->env_steps[1] = (uint16_t)lo2;
        break;
    case 19: {
        int16_t x[4];
        x[0] = (int16_t)-(int)((w1 & 2) >> 1);
        x[1] = (int16_t)-(int)(w1 & 1);
        x[2] = mk ? 0 : (int16_t)-(int)((w1 & 8) >> 1);
        x[3] = mk ? 0 : (int16_t)-(int)((w1 & 4) >> 1);
        op_envmix_nead(a, mk ? 0 : (int)(w1 & 0x10), (w2 >> 20) & 0xFF0, (w2 >> 12) & 0xFF0, (w2 >> 4) & 0xFF0, (w2 << 4) & 0xFF0,
                       (w1 >> 12) & 0xFF0, (w1 >> 8) & 0xFF, x);
        break;
    }
    case 20: op_load(a, w1 & 0xFFF, a24, (w1 >> 12) & 0xFFF); break;
    case 21: op_save(a, w1 & 0xFFF, a24, (w1 >> 12) & 0xFFF); break;
    case 22: a->env_values[0] = (uint16_t)hi2; a->env_values[1] = (uint16_t)lo2; break;
    default: if (!mk) a->unknown++; break;          /* MK : 23-31 = sans effet */
    }
}

/* ------------------------------------------------------------ API */

const char *aspd_abi_name(int abi)
{
    switch (abi) {
    case AUDIO_ABI1: return "ABI1";
    case AUDIO_NEAD_MK: return "NEAD (MK)";
    case AUDIO_NEAD_OOT: return "NEAD";
    }
    return "?";
}

int aspd_detect(const uint8_t *rdram, uint32_t size, uint32_t ucode_data, const char *id)
{
    uint32_t a = ucode_data & 0x7FFFFF, w0, w30;
    if (a + 0x34 > size) return AUDIO_ABI_UNKNOWN;
#define BE(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3])
    w0 = BE(rdram + a);
    w30 = BE(rdram + a + 0x30);
#undef BE
    if (w0 != 1) return AUDIO_ABI_UNKNOWN;           /* ABI 3 (MusyX, Rare...) : non gere */
    if (w30 == 0xF0000F00u) return AUDIO_ABI1;
    if (id && id[0] == 'K' && id[1] == 'T') return AUDIO_NEAD_MK;
    return AUDIO_NEAD_OOT;
}

void aspd_run(Aspd *a, uint8_t *rdram, uint32_t size, int abi, uint32_t alist, uint32_t alist_size)
{
    uint32_t p = alist & 0xFFFFFF, end, n = 0;
    R = rdram; RS = size;
    if (abi == AUDIO_ABI_UNKNOWN) return;
    if (alist_size > 0x10000) alist_size = 0x10000;
    end = p + (alist_size & ~7u);
    memset(a->seg, 0, sizeof a->seg);
    a->lists++;
    while (p < end && n++ < 8192) {
        uint32_t w1 = (uint32_t)d32(p), w2 = (uint32_t)d32(p + 4);
        p += 8;
        a->cmds++;
        if (abi == AUDIO_ABI1) abi1(a, w1, w2);
        else nead(a, abi == AUDIO_NEAD_MK, w1, w2);
    }
}
