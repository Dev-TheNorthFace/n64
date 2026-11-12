/* rdp.c : RDP logiciel. */
#include "rdp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TC_MAX 128               /* taille maxi d'une tuile en cache (texels) */

/* ------------------------------------------------------------ utilitaires */

static inline uint8_t rd8(const Rdp *r, uint32_t a) { a &= 0xFFFFFF; return a < r->rdram_size ? r->rdram[a] : 0; }
static inline uint16_t rd16(const Rdp *r, uint32_t a) { return (uint16_t)(rd8(r, a) << 8 | rd8(r, a + 1)); }
static inline uint32_t rd32(const Rdp *r, uint32_t a) { return (uint32_t)rd16(r, a) << 16 | rd16(r, a + 2); }
static inline void wr16(Rdp *r, uint32_t a, uint16_t v) { a &= 0xFFFFFF; if (a + 1 < r->rdram_size) { r->rdram[a] = (uint8_t)(v >> 8); r->rdram[a + 1] = (uint8_t)v; } }
static inline void wr32(Rdp *r, uint32_t a, uint32_t v) { wr16(r, a, (uint16_t)(v >> 16)); wr16(r, a + 2, (uint16_t)v); }

static inline int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

typedef struct { int r, g, b, a; } C4;

static C4 c4u(uint32_t c) { C4 o = { (int)(c >> 24), (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255) }; return o; }
static C4 c5551(uint16_t p)
{
    C4 o;
    o.r = ((p >> 11) & 31) * 255 / 31; o.g = ((p >> 6) & 31) * 255 / 31; o.b = ((p >> 1) & 31) * 255 / 31;
    o.a = (p & 1) ? 255 : 0;
    return o;
}
static C4 cia16(uint16_t p) { C4 o; o.r = o.g = o.b = p >> 8; o.a = p & 255; return o; }

int rdp_init(Rdp *r, uint8_t *rdram, uint32_t size)
{
    memset(r, 0, sizeof *r);
    r->rdram = rdram;
    r->rdram_size = size;
    r->zbuf = (float *)malloc(sizeof(float) * RDP_MAXW * RDP_MAXH);
    r->tcache = (uint32_t *)malloc(sizeof(uint32_t) * 32 * TC_MAX * TC_MAX);
    if (!r->zbuf || !r->tcache) return -1;
    r->tmem_gen = 1;
    {
        int i;
        for (i = 0; i < RDP_MAXW * RDP_MAXH; i++) r->zbuf[i] = 1.0f;
    }
    r->scx1 = 320; r->scy1 = 240;
    return 0;
}

void rdp_free(Rdp *r) { free(r->zbuf); r->zbuf = NULL; free(r->tcache); r->tcache = NULL; }

int rdp_command_len(uint32_t w0)
{
    uint32_t op = (w0 >> 24) & 0x3F;
    if (op >= 0x08 && op <= 0x0F) return 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
    if (op == 0x24 || op == 0x25) return 2;
    return 1;
}

/* ------------------------------------------------------------ TMEM */

static void load_block(Rdp *r, int t, int uls, int ult, int lrs, int dxt)
{
    RdpTile *tl = &r->tile[t];
    int bpp = r->timg_siz == 0 ? 4 : 8 << (r->timg_siz - 1);     /* bits par texel */
    uint32_t count = (uint32_t)(lrs - uls + 1), bytes, i;
    uint32_t src = r->timg_addr + (uint32_t)((ult * r->timg_w + uls) * bpp / 8);
    uint32_t dst = (uint32_t)tl->tmem * 8;
    uint32_t counter = 0;
    bytes = (count * (uint32_t)bpp + 7) / 8;
    bytes = (bytes + 7) & ~7u;
    if (r->timg_siz == 3) {                       /* RGBA32 : RG en bas, BA en haut de la TMEM */
        for (i = 0; i < bytes / 8; i++) {
            int odd = (counter >> 11) & 1;
            int k;
            for (k = 0; k < 2; k++) {
                uint32_t texel = rd32(r, src + i * 8 + (uint32_t)k * 4);
                uint32_t a = ((dst / 2 + i * 4 + (uint32_t)k * 2) ^ (odd ? 2u : 0u)) & 0x7FF;
                r->tmem[a] = (uint8_t)(texel >> 24); r->tmem[a + 1] = (uint8_t)(texel >> 16);
                r->tmem[a | 0x800] = (uint8_t)(texel >> 8); r->tmem[(a + 1) | 0x800] = (uint8_t)texel;
            }
            counter += (uint32_t)dxt;
        }
    } else {
        for (i = 0; i < bytes / 8; i++) {
            int odd = (counter >> 11) & 1, k;
            for (k = 0; k < 8; k++) {
                uint32_t a = (dst + i * 8 + ((uint32_t)k ^ (odd ? 4u : 0u))) & 0xFFF;
                r->tmem[a] = rd8(r, src + i * 8 + (uint32_t)k);
            }
            counter += (uint32_t)dxt;
        }
    }
    tl->uls = uls << 2; tl->ult = ult << 2; tl->lrs = lrs << 2; tl->lrt = dxt;   /* comme le materiel */
}

static void load_tile(Rdp *r, int t, int uls, int ult, int lrs, int lrt)
{
    RdpTile *tl = &r->tile[t];
    int x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2, x, y;
    int bpp = r->timg_siz == 0 ? 4 : 8 << (r->timg_siz - 1);
    uint32_t line = (uint32_t)tl->line * 8;
    for (y = y0; y <= y1; y++) {
        int odd = (y - y0) & 1;
        uint32_t src = r->timg_addr + (uint32_t)((y * r->timg_w + x0) * bpp / 8);
        uint32_t dst = (uint32_t)tl->tmem * 8 + (uint32_t)(y - y0) * line;
        if (r->timg_siz == 3) {
            for (x = 0; x <= x1 - x0; x++) {
                uint32_t texel = rd32(r, src + (uint32_t)x * 4);
                uint32_t a = (((uint32_t)tl->tmem * 4 + (uint32_t)(y - y0) * line / 2 + (uint32_t)x * 2) ^ (odd ? 2u : 0u)) & 0x7FF;
                r->tmem[a] = (uint8_t)(texel >> 24); r->tmem[a + 1] = (uint8_t)(texel >> 16);
                r->tmem[a | 0x800] = (uint8_t)(texel >> 8); r->tmem[(a + 1) | 0x800] = (uint8_t)texel;
            }
        } else {
            uint32_t n = (uint32_t)((x1 - x0 + 1) * bpp + 7) / 8, i;
            for (i = 0; i < n; i++) r->tmem[((dst + i) ^ (odd ? 4u : 0u)) & 0xFFF] = rd8(r, src + i);
        }
    }
    tl->uls = uls; tl->ult = ult; tl->lrs = lrs; tl->lrt = lrt;
}

static void load_tlut(Rdp *r, int t, int uls, int ult, int lrs)
{
    RdpTile *tl = &r->tile[t];
    int count = ((lrs - uls) >> 2) + 1, i, k;
    uint32_t src = r->timg_addr + (uint32_t)(((ult >> 2) * r->timg_w + (uls >> 2)) * 2);
    uint32_t dst = (uint32_t)tl->tmem * 8;
    for (i = 0; i < count && i < 256; i++) {
        uint16_t e = rd16(r, src + (uint32_t)i * 2);
        for (k = 0; k < 4; k++) {
            uint32_t a = (dst + (uint32_t)i * 8 + (uint32_t)k * 2) & 0xFFF;
            r->tmem[a] = (uint8_t)(e >> 8); r->tmem[a + 1] = (uint8_t)e;
        }
    }
}

static C4 tlut_entry(const Rdp *r, int idx)
{
    uint32_t a = (0x800 + (uint32_t)idx * 8) & 0xFFF;
    uint16_t e = (uint16_t)(r->tmem[a] << 8 | r->tmem[a + 1]);
    return (r->omh & 0x4000) ? cia16(e) : c5551(e);
}

/* lecture d'un texel entier (x,y deja ramenes dans la tuile) */
static C4 texel(const Rdp *r, const RdpTile *t, int x, int y)
{
    uint32_t line = (uint32_t)t->line * 8, base = (uint32_t)t->tmem * 8 + (uint32_t)y * line;
    uint32_t sw = (y & 1) ? 4u : 0u;
    C4 o = { 0, 0, 0, 255 };
    switch (t->siz) {
    case 0: {                                          /* 4 bits */
        uint8_t b = r->tmem[((base + (uint32_t)x / 2) ^ sw) & 0xFFF];
        int n = (x & 1) ? (b & 15) : (b >> 4);
        if (t->fmt == 2) return tlut_entry(r, (t->pal << 4) | n);
        if (t->fmt == 3) { int i = (n >> 1) * 255 / 7; o.r = o.g = o.b = i; o.a = (n & 1) ? 255 : 0; return o; }
        o.r = o.g = o.b = o.a = n * 17;
        return o;
    }
    case 1: {                                          /* 8 bits */
        uint8_t b = r->tmem[((base + (uint32_t)x) ^ sw) & 0xFFF];
        if (t->fmt == 2) return tlut_entry(r, b);
        if (t->fmt == 3) { o.r = o.g = o.b = (b >> 4) * 17; o.a = (b & 15) * 17; return o; }
        o.r = o.g = o.b = o.a = b;
        return o;
    }
    case 2: {                                          /* 16 bits */
        uint32_t a = ((base + (uint32_t)x * 2) ^ sw) & 0xFFF;
        uint16_t p = (uint16_t)(r->tmem[a] << 8 | r->tmem[a + 1]);
        if (t->fmt == 3) return cia16(p);
        if (t->fmt == 2) return tlut_entry(r, p >> 8);
        return c5551(p);
    }
    default: {                                         /* 32 bits */
        uint32_t a = (((uint32_t)t->tmem * 4 + (uint32_t)y * line / 2 + (uint32_t)x * 2) ^ (sw ? 2u : 0u)) & 0x7FF;
        o.r = r->tmem[a]; o.g = r->tmem[a + 1]; o.b = r->tmem[a | 0x800]; o.a = r->tmem[(a + 1) | 0x800];
        return o;
    }
    }
}

static int wrap_coord(int c, int mask, int mirror, int clamp, int maxv)
{
    if (clamp || !mask) {
        if (c < 0) c = 0;
        if (c > maxv) c = maxv;
    }
    if (mask) {
        int m = (1 << mask);
        if (mirror && (c & m)) c = m - 1 - (c & (m - 1));
        else c &= m - 1;
    }
    return c;
}

static float shift_factor(int sh)
{
    if (!sh) return 1.0f;
    if (sh <= 10) return 1.0f / (float)(1 << sh);
    return (float)(1 << (16 - sh));
}

/* tuile decodee une fois en RGBA 8888 : evite le decodage de chaque texel a chaque pixel */
/* cache de textures decodees : 32 entrees (128x128 maxi), reconnues par une
 * empreinte (parametres de tuile + octets de TMEM). Un jeu qui alterne entre
 * plusieurs textures les retrouve sans les redecoder. */
static const uint32_t *tile_cache(Rdp *r, int ti, int *W)
{
    const RdpTile *tl = &r->tile[ti];
    uint32_t tlut = r->omh & 0xC000;
    int maxs = (tl->lrs - tl->uls) >> 2, maxt = (tl->lrt - tl->ult) >> 2, w, h, x, y, e, best;
    uint32_t sig, *out;
    if (!r->tcache) return NULL;
    if (r->tc_gen[ti] == r->tmem_gen && r->tc_tlut[ti] == tlut) {
        e = r->tc_slot[ti];
        if (e < 0) return NULL;
        if (r->ce_sig[e] != r->tc_tsig[ti]) goto lookup;          /* entree reprise par une autre texture */
        r->ce_use[e] = ++r->ce_clock;
        *W = r->ce_w[e];
        return r->tcache + (size_t)e * TC_MAX * TC_MAX;
    }
lookup:
    r->tc_gen[ti] = r->tmem_gen; r->tc_tlut[ti] = tlut;
    if (maxs < 0) maxs = 0;
    if (maxt < 0) maxt = 0;
    w = maxs + 1; h = maxt + 1;
    if (tl->masks && (1 << tl->masks) > w) w = 1 << tl->masks;
    if (tl->maskt && (1 << tl->maskt) > h) h = 1 << tl->maskt;
    if (w > TC_MAX || h > TC_MAX) { r->tc_slot[ti] = -1; return NULL; }       /* trop grande : lecture directe */
    {
        uint32_t a, n, k;
        sig = 2166136261u;
#define MIX(v) sig = (sig ^ (uint32_t)(v)) * 16777619u
        MIX(tl->fmt); MIX(tl->siz); MIX(tl->line); MIX(tl->tmem); MIX(tl->pal); MIX(tl->cms); MIX(tl->cmt);
        MIX(tl->masks); MIX(tl->maskt); MIX(tl->uls); MIX(tl->ult); MIX(tl->lrs); MIX(tl->lrt); MIX(tlut); MIX(w); MIX(h);
        if (tl->siz == 3 || tl->fmt == 2) { a = 0; n = 4096; }             /* RGBA32 / palette : toute la TMEM */
        else { a = (uint32_t)tl->tmem * 8; n = (uint32_t)(tl->line ? tl->line : 1) * 8 * (uint32_t)h; if (n > 4096) n = 4096; }
        for (k = 0; k + 4 <= n; k += 4) {
            uint32_t p = (a + k) & 0xFFC;
            MIX((uint32_t)r->tmem[p] << 24 | (uint32_t)r->tmem[p + 1] << 16 | (uint32_t)r->tmem[p + 2] << 8 | r->tmem[p + 3]);
        }
#undef MIX
        if (sig == 0) sig = 1;
    }
    r->tc_tsig[ti] = sig;
    best = 0;
    for (e = 0; e < 32; e++) {
        if (r->ce_sig[e] == sig) {
            r->tc_slot[ti] = e; r->ce_use[e] = ++r->ce_clock; *W = r->ce_w[e];
            return r->tcache + (size_t)e * TC_MAX * TC_MAX;
        }
        if (r->ce_use[e] < r->ce_use[best]) best = e;
    }
    /* absente : decodage dans l'entree la moins recemment utilisee */
    e = best;
    out = r->tcache + (size_t)e * TC_MAX * TC_MAX;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            C4 c = texel(r, tl, x, y);
            out[y * w + x] = (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | (uint32_t)c.a;
        }
    r->ce_sig[e] = sig; r->ce_w[e] = w; r->ce_h[e] = h; r->ce_use[e] = ++r->ce_clock;
    r->tc_slot[ti] = e;
    *W = w;
    return out;
}

static inline C4 unpack(uint32_t p) { C4 o; o.r = (int)(p >> 24); o.g = (int)((p >> 16) & 255); o.b = (int)((p >> 8) & 255); o.a = (int)(p & 255); return o; }

/* parametres d'une tuile, prepares une fois par primitive */
typedef struct {
    Rdp *r;
    const RdpTile *tl;
    const uint32_t *tc;
    int W, maxs, maxt, cs, ct, ms, mt, bilinear;
    float sfs, sft, offs, offt;
} TexCtx;

static void tex_setup(Rdp *r, int ti, int bilinear, TexCtx *x)
{
    const RdpTile *tl = &r->tile[ti & 7];
    x->r = r; x->tl = tl; x->bilinear = bilinear;
    x->sfs = shift_factor(tl->shifts); x->sft = shift_factor(tl->shiftt);
    x->offs = tl->uls / 4.0f; x->offt = tl->ult / 4.0f;
    x->maxs = (tl->lrs - tl->uls) >> 2; x->maxt = (tl->lrt - tl->ult) >> 2;
    if (x->maxs < 0) x->maxs = 0;
    if (x->maxt < 0) x->maxt = 0;
    x->cs = (tl->cms & 2) != 0; x->ct = (tl->cmt & 2) != 0; x->ms = tl->cms & 1; x->mt = tl->cmt & 1;
    x->W = 0;
    x->tc = tile_cache(r, ti & 7, &x->W);
}

static inline C4 tex_fetch(const TexCtx *x, int u, int v)
{
    return x->tc ? unpack(x->tc[v * x->W + u]) : texel(x->r, x->tl, u, v);
}

/* une seule conversion flottant -> entier par coordonnee (point fixe 1/256) :
 * sur le PPU de la PS3, chaque conversion coute cher (passage par la memoire) */
static C4 sample_ctx(const TexCtx *x, float s, float t)
{
    const RdpTile *tl = x->tl;
    float fs = s * x->sfs - x->offs, ft = t * x->sft - x->offt;
    int is, it;
    if (!(fs > -30000.0f && fs < 30000.0f)) fs = 0;           /* NaN / hors limites */
    if (!(ft > -30000.0f && ft < 30000.0f)) ft = 0;
    /* +2^23 : la conversion tronque un nombre positif = plancher */
    is = (int)(fs * 256.0f + 8388608.0f) - 8388608;
    it = (int)(ft * 256.0f + 8388608.0f) - 8388608;
    if (!x->bilinear) {
        int u = wrap_coord(is >> 8, tl->masks, x->ms, x->cs, x->maxs);
        int v = wrap_coord(it >> 8, tl->maskt, x->mt, x->ct, x->maxt);
        return tex_fetch(x, u, v);
    } else {
        int bs = is - 128, bt = it - 128;
        int x0 = bs >> 8, y0 = bt >> 8, fx = bs & 255, fy = bt & 255;
        int xa = wrap_coord(x0, tl->masks, x->ms, x->cs, x->maxs), xb = wrap_coord(x0 + 1, tl->masks, x->ms, x->cs, x->maxs);
        int ya = wrap_coord(y0, tl->maskt, x->mt, x->ct, x->maxt), yb = wrap_coord(y0 + 1, tl->maskt, x->mt, x->ct, x->maxt);
        C4 a = tex_fetch(x, xa, ya), b = tex_fetch(x, xb, ya), c = tex_fetch(x, xa, yb), d = tex_fetch(x, xb, yb), o;
#define BL(f) ((((a.f * (256 - fx) + b.f * fx) >> 8) * (256 - fy) + ((c.f * (256 - fx) + d.f * fx) >> 8) * fy) >> 8)
        o.r = BL(r); o.g = BL(g); o.b = BL(b); o.a = BL(a);
#undef BL
        return o;
    }
}

/* ------------------------------------------------------------ combiner
 * Les selecteurs sont resolus une fois par primitive en pointeurs vers les
 * valeurs (texels, ombrage, constantes) : la boucle des pixels ne fait plus
 * que des multiplications. */

typedef struct { C4 comb, t0, t1, shade, prim, env, k0, k1, khalf, lod, primlod; } CombIn;
typedef struct { const int *p; int bc; } Src;            /* bc : l'alpha sert pour R, G et B */
typedef struct {
    Src a[2], b[2], c[2], d[2];
    const int *aa[2], *ab[2], *ac[2], *ad[2];
    int two, uses_t1, uses_tex, uses_shade;
} Comb;

static void comb_consts(const Rdp *r, CombIn *in)
{
    in->prim = c4u(r->prim); in->env = c4u(r->env);
    in->k0.r = in->k0.g = in->k0.b = in->k0.a = 0;
    in->k1.r = in->k1.g = in->k1.b = in->k1.a = 255;
    in->khalf.r = in->khalf.g = in->khalf.b = in->khalf.a = 128;
    in->lod = in->k0;
    in->primlod.r = in->primlod.g = in->primlod.b = in->primlod.a = r->prim_lod;
    in->comb = in->k0;
}

static Src rgb_src(CombIn *in, int sel, int kind, int *t1, int *tex, int *sh)
{
    Src s;
    s.bc = 0; s.p = &in->k0.r;
    switch (sel) {
    case 0: s.p = &in->comb.r; return s;
    case 1: s.p = &in->t0.r; *tex = 1; return s;
    case 2: s.p = &in->t1.r; *tex = 1; *t1 = 1; return s;
    case 3: s.p = &in->prim.r; return s;
    case 4: s.p = &in->shade.r; *sh = 1; return s;
    case 5: s.p = &in->env.r; return s;
    }
    if (kind == 0) { if (sel == 6) s.p = &in->k1.r; else if (sel == 7) s.p = &in->khalf.r; return s; }
    if (kind == 1) return s;
    if (kind == 3) { if (sel == 6) s.p = &in->k1.r; return s; }
    s.bc = 1;                                              /* entree C */
    switch (sel) {
    case 7: s.p = &in->comb.r; break;
    case 8: s.p = &in->t0.r; *tex = 1; break;
    case 9: s.p = &in->t1.r; *tex = 1; *t1 = 1; break;
    case 10: s.p = &in->prim.r; break;
    case 11: s.p = &in->shade.r; *sh = 1; break;
    case 12: s.p = &in->env.r; break;
    case 13: s.p = &in->lod.r; break;
    case 14: s.p = &in->primlod.r; break;
    default: s.bc = 0; s.p = &in->k0.r; break;
    }
    return s;
}

static const int *a_src(CombIn *in, int sel, int kind, int *t1, int *tex, int *sh)
{
    switch (sel) {
    case 0: return kind == 2 ? &in->lod.a : &in->comb.a;
    case 1: *tex = 1; return &in->t0.a;
    case 2: *tex = 1; *t1 = 1; return &in->t1.a;
    case 3: return &in->prim.a;
    case 4: *sh = 1; return &in->shade.a;
    case 5: return &in->env.a;
    case 6: return kind == 2 ? &in->primlod.a : &in->k1.a;
    }
    return &in->k0.a;
}

static void comb_setup(const Rdp *r, CombIn *in, Comb *cb)
{
    uint32_t w0 = r->comb_w0, w1 = r->comb_w1;
    int k, t1 = 0, tex = 0, sh = 0;
    comb_consts(r, in);
    cb->two = ((r->omh >> 20) & 3) == 1;
    for (k = 0; k < 2; k++) {
        int A, B, C, D, aA, aB, aC, aD;
        if (k == 0) {
            A = (w0 >> 20) & 15; C = (w0 >> 15) & 31; aA = (w0 >> 12) & 7; aC = (w0 >> 9) & 7;
            B = (w1 >> 28) & 15; D = (w1 >> 15) & 7; aB = (w1 >> 12) & 7; aD = (w1 >> 9) & 7;
        } else {
            A = (w0 >> 5) & 15; C = w0 & 31; aA = (w1 >> 21) & 7; aC = (w1 >> 18) & 7;
            B = (w1 >> 24) & 15; D = (w1 >> 6) & 7; aB = (w1 >> 3) & 7; aD = w1 & 7;
        }
        if (k == 1 && !cb->two) { cb->a[1] = cb->a[0]; cb->b[1] = cb->b[0]; cb->c[1] = cb->c[0]; cb->d[1] = cb->d[0];
                                  cb->aa[1] = cb->aa[0]; cb->ab[1] = cb->ab[0]; cb->ac[1] = cb->ac[0]; cb->ad[1] = cb->ad[0]; break; }
        cb->a[k] = rgb_src(in, A, 0, &t1, &tex, &sh); cb->b[k] = rgb_src(in, B, 1, &t1, &tex, &sh);
        cb->c[k] = rgb_src(in, C, 2, &t1, &tex, &sh); cb->d[k] = rgb_src(in, D, 3, &t1, &tex, &sh);
        cb->aa[k] = a_src(in, aA, 0, &t1, &tex, &sh); cb->ab[k] = a_src(in, aB, 1, &t1, &tex, &sh);
        cb->ac[k] = a_src(in, aC, 2, &t1, &tex, &sh); cb->ad[k] = a_src(in, aD, 3, &t1, &tex, &sh);
    }
    cb->uses_t1 = t1; cb->uses_tex = tex; cb->uses_shade = sh;
}

#define SRCV(s, ch) ((s).bc ? (s).p[3] : (s).p[ch])

static inline C4 comb_cycle(const Comb *cb, int k)
{
    C4 o;
    int ch, v[3];
    for (ch = 0; ch < 3; ch++) {
        int x = ((SRCV(cb->a[k], ch) - SRCV(cb->b[k], ch)) * SRCV(cb->c[k], ch)) / 255 + SRCV(cb->d[k], ch);
        v[ch] = x < 0 ? 0 : x > 255 ? 255 : x;
    }
    o.r = v[0]; o.g = v[1]; o.b = v[2];
    {
        int x = ((*cb->aa[k] - *cb->ab[k]) * *cb->ac[k]) / 255 + *cb->ad[k];
        o.a = x < 0 ? 0 : x > 255 ? 255 : x;
    }
    return o;
}

static inline C4 comb_run(const Comb *cb, CombIn *in)
{
    C4 c = comb_cycle(cb, 0);
    if (cb->two) { in->comb = c; c = comb_cycle(cb, 1); }
    return c;
}

/* ------------------------------------------------------------ ecriture des pixels */

static C4 read_pixel(const Rdp *r, int x, int y)
{
    if (r->cimg_siz == 3) return c4u(rd32(r, r->cimg_addr + (uint32_t)(y * r->cimg_w + x) * 4));
    return c5551(rd16(r, r->cimg_addr + (uint32_t)(y * r->cimg_w + x) * 2));
}

static void write_pixel(Rdp *r, int x, int y, C4 c)
{
    if (r->cimg_siz == 3)
        wr32(r, r->cimg_addr + (uint32_t)(y * r->cimg_w + x) * 4, (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | 255u);
    else
        wr16(r, r->cimg_addr + (uint32_t)(y * r->cimg_w + x) * 2,
             (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 3) << 6) | ((c.b >> 3) << 1) | 1));
}

/* melangeur simplifie : opaque, translucide (CLR_IN*A + MEM*(1-A)) ou brouillard */
static int blend_mode(const Rdp *r, int *fog)
{
    uint32_t l = r->oml;
    int cyc2 = ((r->omh >> 20) & 3) == 1;
    int m1a = (l >> 30) & 3, m1b = (l >> 26) & 3, m2a = (l >> 22) & 3, m2b = (l >> 18) & 3;
    *fog = 0;
    if (cyc2 && m1a == 3 && m1b == 2) {                 /* cycle 0 : brouillard, cycle 1 = surface */
        *fog = 1;
        m1a = (l >> 28) & 3; m1b = (l >> 24) & 3; m2a = (l >> 20) & 3; m2b = (l >> 16) & 3;
    }
    if (m1a == 0 && m1b == 0 && m2a == 1 && m2b == 0 && (l & (0x4000 | 0x40))) return 1;
    return 0;
}

static void ensure_zbuf(Rdp *r)
{
    if (r->zbuf_for != r->zimg_addr) { int i; for (i = 0; i < RDP_MAXW * RDP_MAXH; i++) r->zbuf[i] = 1.0f; r->zbuf_for = r->zimg_addr; }
}

/* ------------------------------------------------------------ triangles */

static inline void blend_px(Rdp *r, int x, int y, C4 *c)
{
    C4 d = read_pixel(r, x, y);
    c->r = (c->r * c->a + d.r * (255 - c->a)) / 255;
    c->g = (c->g * c->a + d.g * (255 - c->a)) / 255;
    c->b = (c->b * c->a + d.b * (255 - c->a)) / 255;
}

/* intervalle de x (pixels) ou f0 + d*x >= 0 */
static inline void span_limit(float f0, float d, int *lo, int *hi)
{
    if (d > 1e-12f) { float x = -f0 / d; if (x > (float)*lo) *lo = x > 1e6f ? 1000000 : (int)floorf(x); }
    else if (d < -1e-12f) { float x = -f0 / d; if (x < (float)*hi) *hi = x < -1e6f ? -1000000 : (int)ceilf(x) + 1; }
    else if (f0 < 0) *hi = *lo;
}

void rdp_triangle(Rdp *r, const RdpVtx v[3], int textured, int ti, int smooth)
{
    float x0 = v[0].x, y0 = v[0].y, x1 = v[1].x, y1 = v[1].y, x2 = v[2].x, y2 = v[2].y;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    int minx, maxx, miny, maxy, x, y, cycle = (r->omh >> 20) & 3;
    int zcmp = (r->oml & 0x10) != 0, zupd = (r->oml & 0x20) != 0, zprim = (r->oml & 4) != 0;
    int bilinear = (r->omh & 0x2000) != 0 && !r->fast, fogon, blend = blend_mode(r, &fogon);
    int acmp = (r->oml & 3) == 1, cvgsel = (r->oml & 0x2000) != 0, persp = (r->omh & 0x80000) != 0 && r->fast < 2;
    float ia, dw0x, dw1x;
    float av[8][3], d0[8], d1[8], adx[8];
    int64_t IX0, IY0, IX1, IY1, IX2, IY2, IA, dE0, dE1;
    int32_t sh[4], dsh[4];
    float fxs;
    TexCtx tx0, tx1;
    CombIn in;
    Comb cb;
    C4 fc;
    int k;
    if (!(fabsf(area) >= 1e-6f) || r->cimg_w <= 0) return;
    if (cycle >= 2) cycle = 0;
    ia = 1.0f / area;
    {
        float fx0 = fminf(x0, fminf(x1, x2)), fx1 = fmaxf(x0, fmaxf(x1, x2));
        float fy0 = fminf(y0, fminf(y1, y2)), fy1 = fmaxf(y0, fmaxf(y1, y2));
        if (!(fx1 >= 0) || !(fy1 >= 0) || !(fx0 < RDP_MAXW) || !(fy0 < RDP_MAXH)) return;   /* aussi NaN */
        if (fx0 < 0) fx0 = 0;
        if (fy0 < 0) fy0 = 0;
        if (fx1 > RDP_MAXW) fx1 = RDP_MAXW;
        if (fy1 > RDP_MAXH) fy1 = RDP_MAXH;
        minx = (int)floorf(fx0); maxx = (int)ceilf(fx1); miny = (int)floorf(fy0); maxy = (int)ceilf(fy1);
    }
    if (minx < r->scx0) minx = r->scx0;
    if (miny < r->scy0) miny = r->scy0;
    if (maxx > r->scx1) maxx = r->scx1;
    if (maxy > r->scy1) maxy = r->scy1;
    if (maxx > r->cimg_w) maxx = r->cimg_w;
    if (maxx > RDP_MAXW) maxx = RDP_MAXW;
    if (maxy > RDP_MAXH) maxy = RDP_MAXH;
    if (minx >= maxx || miny >= maxy) return;
    if (zcmp || zupd) ensure_zbuf(r);
    r->tris++;

    comb_setup(r, &in, &cb);
    if (!textured) cb.uses_tex = cb.uses_t1 = 0;
    fc = c4u(r->fog);
    if (cb.uses_tex) {
        tex_setup(r, ti, bilinear, &tx0);
        if (cycle == 1 && cb.uses_t1) tex_setup(r, ti + 1, bilinear, &tx1);
    }
    /* plans des attributs : A(x) = A_ligne + x * A_dx */
    dw0x = (y1 - y2) * ia;
    dw1x = (y2 - y0) * ia;
    for (k = 0; k < 3; k++) {
        float iw = v[k].iw;
        av[0][k] = v[k].z; av[1][k] = iw;
        av[2][k] = smooth ? v[k].r : v[0].r; av[3][k] = smooth ? v[k].g : v[0].g;
        av[4][k] = smooth ? v[k].b : v[0].b; av[5][k] = smooth ? v[k].a : v[0].a;
        av[6][k] = persp ? v[k].s * iw : v[k].s; av[7][k] = persp ? v[k].t * iw : v[k].t;
    }
    for (k = 0; k < 8; k++) {
        d0[k] = av[k][0] - av[k][2]; d1[k] = av[k][1] - av[k][2];
        adx[k] = d0[k] * dw0x + d1[k] * dw1x;
    }
    for (k = 0; k < 4; k++) {
        float d = adx[2 + k] * 65536.0f;
        dsh[k] = d > 4194304.0f ? 4194304 : d < -4194304.0f ? -4194304 : (int32_t)d;   /* +-64 niveaux/pixel */
    }
    in.t0 = in.k0; in.t1 = in.k0;
    /* test d'appartenance exact : sommets arrondis au 1/16 de pixel, aretes en
     * entiers 64 bits. Deux triangles qui partagent une arete couvrent ainsi
     * exactement tous les pixels (pas de trous entre eux). */
    {
        float lim = 1.0e6f;                    /* le microcode decoupe a 3 ecrans : jamais atteint */
        for (k = 0; k < 3; k++) if (!(fabsf(v[k].x) < lim && fabsf(v[k].y) < lim)) return;
    }
    IX0 = (int64_t)lrintf(x0 * 16); IY0 = (int64_t)lrintf(y0 * 16);
    IX1 = (int64_t)lrintf(x1 * 16); IY1 = (int64_t)lrintf(y1 * 16);
    IX2 = (int64_t)lrintf(x2 * 16); IY2 = (int64_t)lrintf(y2 * 16);
    IA = (IX1 - IX0) * (IY2 - IY0) - (IX2 - IX0) * (IY1 - IY0);
    if (IA == 0) return;
    dE0 = 16 * (IY1 - IY2);
    dE1 = 16 * (IY2 - IY0);
    for (y = miny; y < maxy; y++) {
        float py = (float)y + 0.5f;
        int64_t ipy = (int64_t)y * 16 + 8, ipx, E0, E1;
        float w0r = ((x1 - 0.5f) * (y2 - py) - (x2 - 0.5f) * (y1 - py)) * ia;     /* en x = 0 */
        float w1r = ((x2 - 0.5f) * (y0 - py) - (x0 - 0.5f) * (y2 - py)) * ia;
        float arow[8];
        int lo = minx, hi = maxx;
        /* portion de la ligne a l'interieur des 3 aretes (+1 pixel de marge) */
        span_limit(w0r, dw0x, &lo, &hi);
        span_limit(w1r, dw1x, &lo, &hi);
        span_limit(1.0f - w0r - w1r, -dw0x - dw1x, &lo, &hi);
        if (lo < minx) lo = minx;
        if (hi > maxx) hi = maxx;
        if (lo >= hi) continue;
        for (k = 0; k < 8; k++) arow[k] = av[k][2] + w0r * d0[k] + w1r * d1[k];
        ipx = (int64_t)lo * 16 + 8;
        E0 = (IX1 - ipx) * (IY2 - ipy) - (IX2 - ipx) * (IY1 - ipy);
        E1 = (IX2 - ipx) * (IY0 - ipy) - (IX0 - ipx) * (IY2 - ipy);
        fxs = (float)lo;
        /* ombrage en point fixe 16.16 : pas de conversion par pixel */
        for (k = 0; k < 4; k++) {
            float v0 = arow[2 + k] + adx[2 + k] * fxs;
            v0 = v0 < -4096.0f ? -4096.0f : v0 > 4096.0f ? 4096.0f : v0;
            sh[k] = (int32_t)(v0 * 65536.0f);
        }
        for (x = lo; x < hi; x++, E0 += dE0, E1 += dE1, fxs += 1.0f, sh[0] += dsh[0], sh[1] += dsh[1], sh[2] += dsh[2], sh[3] += dsh[3]) {
            float fx = fxs;
            float z;
            C4 c;
            if (IA > 0 ? (E0 < 0 || E1 < 0 || IA - E0 - E1 < 0) : (E0 > 0 || E1 > 0 || IA - E0 - E1 > 0)) continue;
            z = zprim ? (r->prim_z / 32767.0f) : arow[0] + adx[0] * fx;
            if (zcmp && z > r->zbuf[y * RDP_MAXW + x]) continue;
            if (cb.uses_shade || fogon) {
                in.shade.r = clampi(sh[0] >> 16, 0, 255); in.shade.g = clampi(sh[1] >> 16, 0, 255);
                in.shade.b = clampi(sh[2] >> 16, 0, 255); in.shade.a = clampi(sh[3] >> 16, 0, 255);
            }
            if (cb.uses_tex) {
                float s = arow[6] + adx[6] * fx, t = arow[7] + adx[7] * fx;
                if (persp) { float iw = arow[1] + adx[1] * fx; if (iw > 1e-12f) { float q = 1.0f / iw; s *= q; t *= q; } }
                in.t0 = sample_ctx(&tx0, s, t);
                in.t1 = (cycle == 1 && cb.uses_t1) ? sample_ctx(&tx1, s, t) : in.t0;
            }
            c = comb_run(&cb, &in);
            if (acmp && c.a < (int)(r->blend & 255)) continue;
            if (cvgsel && c.a < 128) continue;
            if (fogon) {
                int f = in.shade.a;
                c.r = (c.r * (255 - f) + fc.r * f) / 255;
                c.g = (c.g * (255 - f) + fc.g * f) / 255;
                c.b = (c.b * (255 - f) + fc.b * f) / 255;
            }
            if (blend) blend_px(r, x, y, &c);
            write_pixel(r, x, y, c);
            if (zupd) r->zbuf[y * RDP_MAXW + x] = z;
            r->pixels++;
        }
    }
}

/* ------------------------------------------------------------ rectangles */

static void fill_rect(Rdp *r, int ulx, int uly, int lrx, int lry)
{
    int cycle = (r->omh >> 20) & 3, x, y;
    if (cycle == 3) { lrx++; lry++; }
    if (ulx < r->scx0) ulx = r->scx0;
    if (uly < r->scy0) uly = r->scy0;
    if (lrx > r->scx1) lrx = r->scx1;
    if (lry > r->scy1) lry = r->scy1;
    if (lrx > r->cimg_w) lrx = r->cimg_w;
    if (lrx > RDP_MAXW) lrx = RDP_MAXW;
    if (lry > RDP_MAXH) lry = RDP_MAXH;
    if (ulx < 0) ulx = 0;
    if (uly < 0) uly = 0;
    r->fills++;
    if (r->cimg_addr == r->zimg_addr) {                /* effacement du tampon Z */
        r->zbuf_for = r->zimg_addr;
        for (y = uly; y < lry; y++) for (x = ulx; x < lrx; x++) r->zbuf[y * RDP_MAXW + x] = 1.0f;
    }
    if (cycle == 3) {
        for (y = uly; y < lry; y++)
            for (x = ulx; x < lrx; x++) {
                if (r->cimg_siz == 3) wr32(r, r->cimg_addr + (uint32_t)(y * r->cimg_w + x) * 4, r->fill);
                else wr16(r, r->cimg_addr + (uint32_t)(y * r->cimg_w + x) * 2, (uint16_t)((x & 1) ? r->fill : r->fill >> 16));
            }
    } else {
        CombIn in;
        Comb cb;
        int fogon, blend = blend_mode(r, &fogon);
        C4 c0;
        comb_setup(r, &in, &cb);
        in.t0 = in.t1 = in.k0;
        in.shade = in.k1;
        c0 = comb_run(&cb, &in);                       /* couleur constante sur tout le rectangle */
        for (y = uly; y < lry; y++)
            for (x = ulx; x < lrx; x++) {
                C4 c = c0;
                if (blend) blend_px(r, x, y, &c);
                write_pixel(r, x, y, c);
            }
    }
}

static void tex_rect(Rdp *r, uint32_t w0, uint32_t w1, uint32_t ex0, uint32_t ex1, int flip)
{
    float ulx = (float)((w1 >> 12) & 0xFFF) / 4, uly = (float)(w1 & 0xFFF) / 4;
    float lrx = (float)((w0 >> 12) & 0xFFF) / 4, lry = (float)(w0 & 0xFFF) / 4;
    int ti = (w1 >> 24) & 7, cycle = (r->omh >> 20) & 3, x, y, x0, y0, x1, y1;
    float s0 = (int16_t)(ex0 >> 16) / 32.0f, t0 = (int16_t)(ex0 & 0xFFFF) / 32.0f;
    float dsdx = (int16_t)(ex1 >> 16) / 1024.0f, dtdy = (int16_t)(ex1 & 0xFFFF) / 1024.0f;
    int bilinear = (r->omh & 0x2000) != 0 && cycle < 2 && !r->fast, fogon, blend = blend_mode(r, &fogon);
    int acmp = (r->oml & 3) == 1 || (r->oml & 0x2000);
    CombIn in;
    Comb cb;
    TexCtx tx0, tx1;
    (void)fogon;
    if (cycle == 2) { dsdx /= 4; lrx += 1; lry += 1; }   /* mode COPY : 4 texels par horloge */
    x0 = (int)ulx; y0 = (int)uly; x1 = (int)lrx; y1 = (int)lry;
    if (x0 < r->scx0) x0 = r->scx0;
    if (y0 < r->scy0) y0 = r->scy0;
    if (x1 > r->scx1) x1 = r->scx1;
    if (y1 > r->scy1) y1 = r->scy1;
    if (x1 > r->cimg_w) x1 = r->cimg_w;
    if (x1 > RDP_MAXW) x1 = RDP_MAXW;
    if (y1 > RDP_MAXH) y1 = RDP_MAXH;
    r->rects++;
    comb_setup(r, &in, &cb);
    in.shade = in.k1;
    tex_setup(r, ti, bilinear, &tx0);
    if (cycle == 1 && cb.uses_t1) tex_setup(r, ti + 1, bilinear, &tx1);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float fx = (float)x - ulx, fy = (float)y - uly;
            float s = flip ? s0 + fy * dsdx : s0 + fx * dsdx;
            float t = flip ? t0 + fx * dtdy : t0 + fy * dtdy;
            C4 c;
            in.t0 = sample_ctx(&tx0, s, t);
            in.t1 = (cycle == 1 && cb.uses_t1) ? sample_ctx(&tx1, s, t) : in.t0;
            if (cycle == 2) c = in.t0;
            else c = comb_run(&cb, &in);
            if (acmp && c.a < 128) continue;
            if (blend && cycle != 2) blend_px(r, x, y, &c);
            write_pixel(r, x, y, c);
        }
}

/* ------------------------------------------------------------ commandes */

void rdp_command(Rdp *r, uint32_t w0, uint32_t w1, uint32_t ex0, uint32_t ex1)
{
    uint32_t op = (w0 >> 24) & 0x3F;
    if (op >= 0x08 && op <= 0x0F) { r->raw_tris++; return; }   /* triangles bruts : non geres (rares en HLE) */
    switch (op) {
    case 0x24: case 0x25:                              /* texture rectangle (+ flip) */
        tex_rect(r, w0, w1, ex0, ex1, op == 0x25);
        break;
    case 0x29: r->full_sync = 1; break;
    case 0x2D:                                          /* scissor */
        r->scx0 = (int)((w0 >> 12) & 0xFFF) >> 2; r->scy0 = (int)(w0 & 0xFFF) >> 2;
        r->scx1 = (int)((w1 >> 12) & 0xFFF) >> 2; r->scy1 = (int)(w1 & 0xFFF) >> 2;
        break;
    case 0x2E: r->prim_z = (uint16_t)(w1 >> 16); break;
    case 0x2F: r->omh = w0 & 0xFFFFFF; r->oml = w1; break;
    case 0x30: r->tmem_gen++; load_tlut(r, (w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF); break;
    case 0x32: {                                        /* set tile size */
        r->tmem_gen++;
        RdpTile *t = &r->tile[(w1 >> 24) & 7];
        t->uls = (w0 >> 12) & 0xFFF; t->ult = w0 & 0xFFF; t->lrs = (w1 >> 12) & 0xFFF; t->lrt = w1 & 0xFFF;
        break;
    }
    case 0x33: r->tmem_gen++; load_block(r, (w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF); break;
    case 0x34: r->tmem_gen++; load_tile(r, (w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF); break;
    case 0x35: {                                        /* set tile */
        r->tmem_gen++;
        RdpTile *t = &r->tile[(w1 >> 24) & 7];
        t->fmt = (w0 >> 21) & 7; t->siz = (w0 >> 19) & 3; t->line = (w0 >> 9) & 0x1FF; t->tmem = w0 & 0x1FF;
        t->pal = (w1 >> 20) & 15; t->cmt = (w1 >> 18) & 3; t->maskt = (w1 >> 14) & 15; t->shiftt = (w1 >> 10) & 15;
        t->cms = (w1 >> 8) & 3; t->masks = (w1 >> 4) & 15; t->shifts = w1 & 15;
        break;
    }
    case 0x36: fill_rect(r, (int)((w1 >> 12) & 0xFFF) >> 2, (int)(w1 & 0xFFF) >> 2, (int)((w0 >> 12) & 0xFFF) >> 2, (int)(w0 & 0xFFF) >> 2); break;
    case 0x37: r->fill = w1; break;
    case 0x38: r->fog = w1; break;
    case 0x39: r->blend = w1; break;
    case 0x3A: r->prim = w1; r->prim_lod = (uint8_t)w0; break;
    case 0x3B: r->env = w1; break;
    case 0x3C: r->comb_w0 = w0 & 0xFFFFFF; r->comb_w1 = w1; break;
    case 0x3D: r->timg_addr = w1 & 0xFFFFFF; r->timg_siz = (w0 >> 19) & 3; r->timg_w = (int)(w0 & 0x3FF) + 1; break;
    case 0x3E: r->zimg_addr = w1 & 0xFFFFFF; break;
    case 0x3F: r->cimg_addr = w1 & 0xFFFFFF; r->cimg_siz = (w0 >> 19) & 3; r->cimg_w = (int)(w0 & 0x3FF) + 1; break;
    default: break;                                     /* syncs, cles, conversion : sans effet ici */
    }
}
