/* gsp.c : microcode graphique en HLE (F3D / F3DEX / F3DEX2). */
#include "gsp.h"
#include <math.h>
#include <string.h>

static inline uint8_t g8(const Gsp *g, uint32_t a) { a &= 0x7FFFFF; return a < g->rdram_size ? g->rdram[a] : 0; }
static inline uint16_t g16(const Gsp *g, uint32_t a) { return (uint16_t)(g8(g, a) << 8 | g8(g, a + 1)); }
static inline uint32_t g32(const Gsp *g, uint32_t a) { return (uint32_t)g16(g, a) << 16 | g16(g, a + 2); }

static uint32_t seg_addr(const Gsp *g, uint32_t a)
{
    return (g->seg[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0x7FFFFF;
}

const char *gsp_ucode_name(int u)
{
    return u == UCODE_F3DEX2 ? "F3DEX2" : u == UCODE_F3DEX ? "F3DEX" : "F3D";
}

static void ident(float m[4][4])
{
    int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) m[i][j] = i == j ? 1.0f : 0.0f;
}

static void mul(float o[4][4], float a[4][4], float b[4][4])   /* o = a*b */
{
    float t[4][4];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(o, t, sizeof t);
}

void gsp_init(Gsp *g, uint8_t *rdram, uint32_t size, Rdp *rdp)
{
    memset(g, 0, sizeof *g);
    g->rdram = rdram; g->rdram_size = size; g->rdp = rdp;
    ident(g->proj); ident(g->mv[0]); ident(g->mvp);
    g->vp_scale[0] = 160; g->vp_scale[1] = 120; g->vp_scale[2] = 511;
    g->vp_trans[0] = 160; g->vp_trans[1] = 120; g->vp_trans[2] = 511;
    g->tex_s = g->tex_t = 1.0f;
}

static int contains(const uint8_t *p, uint32_t n, const char *s)
{
    uint32_t L = (uint32_t)strlen(s), i;
    if (n < L) return 0;
    for (i = 0; i + L <= n; i++) if (!memcmp(p + i, s, L)) return 1;
    return 0;
}

int gsp_detect_ucode(const uint8_t *rdram, uint32_t size, uint32_t data_addr, uint32_t data_size)
{
    const uint8_t *p;
    data_addr &= 0x7FFFFF;
    if (data_size == 0 || data_size > 0x1000) data_size = 0x800;
    if (data_addr >= size) return UCODE_F3D;
    if (data_addr + data_size > size) data_size = size - data_addr;
    p = rdram + data_addr;
    if (contains(p, data_size, "F3DEX2") || contains(p, data_size, "F3DZEX") || contains(p, data_size, "F3DLX2")) return UCODE_F3DEX2;
    if (contains(p, data_size, "F3DEX") || contains(p, data_size, "F3DLX") || contains(p, data_size, "F3DLP")) return UCODE_F3DEX;
    return UCODE_F3D;
}

/* ------------------------------------------------------------ matrices */

static void load_mtx(const Gsp *g, uint32_t a, float m[4][4])
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            int16_t hi = (int16_t)g16(g, a + (uint32_t)(i * 4 + j) * 2);
            uint16_t lo = g16(g, a + 32 + (uint32_t)(i * 4 + j) * 2);
            m[i][j] = (float)hi + (float)lo / 65536.0f;
        }
}

static void do_mtx(Gsp *g, uint32_t addr, int proj, int load, int push)
{
    float m[4][4];
    load_mtx(g, seg_addr(g, addr), m);
    if (proj) {
        if (load) memcpy(g->proj, m, sizeof m);
        else mul(g->proj, m, g->proj);
    } else {
        if (push && g->mv_top < GSP_MTX_DEPTH - 1) { memcpy(g->mv[g->mv_top + 1], g->mv[g->mv_top], sizeof m); g->mv_top++; }
        if (load) memcpy(g->mv[g->mv_top], m, sizeof m);
        else mul(g->mv[g->mv_top], m, g->mv[g->mv_top]);
    }
    g->mvp_dirty = 1;
}

static void pop_mtx(Gsp *g, int n)
{
    while (n-- > 0 && g->mv_top > 0) g->mv_top--;
    g->mvp_dirty = 1;
}

/* ------------------------------------------------------------ sommets */

static void load_vertices(Gsp *g, uint32_t addr, int v0, int n)
{
    int i;
    float (*M)[4] = g->mv[g->mv_top];
    uint32_t a = seg_addr(g, addr);
    if (g->mvp_dirty) { mul(g->mvp, g->mv[g->mv_top], g->proj); g->mvp_dirty = 0; }
    for (i = 0; i < n; i++, a += 16) {
        GspVtx *v;
        float x, y, z;
        uint8_t c0, c1, c2, c3;
        if (v0 + i < 0 || v0 + i >= 80) continue;
        v = &g->v[v0 + i];
        x = (int16_t)g16(g, a); y = (int16_t)g16(g, a + 2); z = (int16_t)g16(g, a + 4);
        v->x = x * g->mvp[0][0] + y * g->mvp[1][0] + z * g->mvp[2][0] + g->mvp[3][0];
        v->y = x * g->mvp[0][1] + y * g->mvp[1][1] + z * g->mvp[2][1] + g->mvp[3][1];
        v->z = x * g->mvp[0][2] + y * g->mvp[1][2] + z * g->mvp[2][2] + g->mvp[3][2];
        v->w = x * g->mvp[0][3] + y * g->mvp[1][3] + z * g->mvp[2][3] + g->mvp[3][3];
        v->s = (int16_t)g16(g, a + 8) * g->tex_s / 32.0f;
        v->t = (int16_t)g16(g, a + 10) * g->tex_t / 32.0f;
        c0 = g8(g, a + 12); c1 = g8(g, a + 13); c2 = g8(g, a + 14); c3 = g8(g, a + 15);
        v->a = c3;
        if (g->geom & GM_LIGHTING) {
            float nx = (int8_t)c0, ny = (int8_t)c1, nz = (int8_t)c2, tx, ty, tz, len, r, gg, b;
            int k;
            tx = nx * M[0][0] + ny * M[1][0] + nz * M[2][0];
            ty = nx * M[0][1] + ny * M[1][1] + nz * M[2][1];
            tz = nx * M[0][2] + ny * M[1][2] + nz * M[2][2];
            len = sqrtf(tx * tx + ty * ty + tz * tz);
            if (len > 1e-6f) { tx /= len; ty /= len; tz /= len; }
            r = g->light[g->num_lights].r; gg = g->light[g->num_lights].g; b = g->light[g->num_lights].b;
            for (k = 0; k < g->num_lights; k++) {
                float d = tx * g->light[k].x + ty * g->light[k].y + tz * g->light[k].z;
                if (d > 0) { r += g->light[k].r * d; gg += g->light[k].g * d; b += g->light[k].b * d; }
            }
            v->r = r > 255 ? 255 : r; v->g = gg > 255 ? 255 : gg; v->b = b > 255 ? 255 : b;
            if (g->geom & GM_TEXGEN) {                       /* placage d'environnement */
                if (g->geom & GM_TEXGEN_LIN) {
                    v->s = acosf(-tx) / 3.14159265f * 1024.0f * g->tex_s / 32.0f;
                    v->t = acosf(-ty) / 3.14159265f * 1024.0f * g->tex_t / 32.0f;
                } else {
                    v->s = (tx + 1.0f) * 512.0f * g->tex_s / 32.0f;
                    v->t = (1.0f - ty) * 512.0f * g->tex_t / 32.0f;
                }
            }
        } else {
            v->r = c0; v->g = c1; v->b = c2;
        }
        if (g->geom & GM_FOG) {
            float f = v->w > 0.001f ? v->z / v->w : -1.0f;
            float fa = f * g->fog_m + g->fog_o;
            v->a = fa < 0 ? 0 : fa > 255 ? 255 : fa;
        }
    }
}

/* ------------------------------------------------------------ triangles */

static void to_screen(const Gsp *g, const GspVtx *c, RdpVtx *o)
{
    float iw = 1.0f / c->w;
    o->x = g->vp_trans[0] + c->x * iw * g->vp_scale[0];
    o->y = g->vp_trans[1] - c->y * iw * g->vp_scale[1];
    o->z = (g->vp_trans[2] + c->z * iw * g->vp_scale[2]) / 1024.0f;
    if (o->z < 0) o->z = 0;
    if (o->z > 1) o->z = 1;
    o->iw = iw;
    o->r = c->r; o->g = c->g; o->b = c->b; o->a = c->a;
    o->s = c->s; o->t = c->t;
}

static GspVtx lerpv(const GspVtx *a, const GspVtx *b, float t)
{
    GspVtx o;
    o.x = a->x + (b->x - a->x) * t; o.y = a->y + (b->y - a->y) * t;
    o.z = a->z + (b->z - a->z) * t; o.w = a->w + (b->w - a->w) * t;
    o.r = a->r + (b->r - a->r) * t; o.g = a->g + (b->g - a->g) * t;
    o.b = a->b + (b->b - a->b) * t; o.a = a->a + (b->a - a->a) * t;
    o.s = a->s + (b->s - a->s) * t; o.t = a->t + (b->t - a->t) * t;
    return o;
}

static void raster(Gsp *g, const GspVtx *a, const GspVtx *b, const GspVtx *c)
{
    RdpVtx v[3];
    to_screen(g, a, &v[0]); to_screen(g, b, &v[1]); to_screen(g, c, &v[2]);
    rdp_triangle(g->rdp, v, g->tex_on, g->tex_tile, (g->geom & GM_SMOOTH) != 0);
}

#define GUARD 3.0f                    /* bande de garde : 3 fois l'ecran */

/* distance signee au plan p (>= 0 : a garder) */
static float plane_dist(const GspVtx *v, int p)
{
    switch (p) {
    case 0: return v->w - 0.01f;
    case 1: return GUARD * v->w - v->x;
    case 2: return GUARD * v->w + v->x;
    case 3: return GUARD * v->w - v->y;
    default: return GUARD * v->w + v->y;
    }
}

static int inside_all(const GspVtx *v)
{
    return v->w > 0.01f && v->x <= GUARD * v->w && -v->x <= GUARD * v->w && v->y <= GUARD * v->w && -v->y <= GUARD * v->w;
}

static void triangle(Gsp *g, int i0, int i1, int i2)
{
    const GspVtx *a, *b, *c;
    if (i0 >= 80 || i1 >= 80 || i2 >= 80 || i0 < 0 || i1 < 0 || i2 < 0) return;
    a = &g->v[i0]; b = &g->v[i1]; c = &g->v[i2];
    g->tris_in++;
    /* rejet trivial si tout est hors d'un meme plan */
    if ((a->x < -a->w && b->x < -b->w && c->x < -c->w) || (a->x > a->w && b->x > b->w && c->x > c->w) ||
        (a->y < -a->w && b->y < -b->w && c->y < -c->w) || (a->y > a->w && b->y > b->w && c->y > c->w) ||
        (a->w <= 0 && b->w <= 0 && c->w <= 0)) { g->tris_culled++; return; }
    /* elimination des faces (avant d'eventuellement decouper) */
    if ((g->geom & (GM_CULLFRONT | GM_CULLBACK)) && a->w > 0 && b->w > 0 && c->w > 0) {
        float ax = a->x / a->w, ay = a->y / a->w, bx = b->x / b->w, by = b->y / b->w, cx = c->x / c->w, cy = c->y / c->w;
        float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);     /* > 0 : sens direct = face avant */
        if ((area < 0 && (g->geom & GM_CULLBACK)) || (area > 0 && (g->geom & GM_CULLFRONT)) || area == 0) { g->tris_culled++; return; }
    }
    if (inside_all(a) && inside_all(b) && inside_all(c)) { raster(g, a, b, c); return; }
    /* decoupage : plan proche (w) puis bande de garde en x et y, pour garder
     * des coordonnees d'ecran raisonnables (precision du rasteriseur) */
    {
        GspVtx poly[2][12];
        int n = 3, p, cur = 0, i;
        poly[0][0] = *a; poly[0][1] = *b; poly[0][2] = *c;
        for (p = 0; p < 5 && n >= 3; p++) {
            int m = 0;
            for (i = 0; i < n; i++) {
                const GspVtx *P = &poly[cur][i], *Q = &poly[cur][(i + 1) % n];
                float dp = plane_dist(P, p), dq = plane_dist(Q, p);
                if (dp >= 0) { if (m < 12) poly[cur ^ 1][m++] = *P; }
                if ((dp >= 0) != (dq >= 0) && m < 12) poly[cur ^ 1][m++] = lerpv(P, Q, dp / (dp - dq));
            }
            n = m; cur ^= 1;
        }
        g->tris_clipped++;
        if (n < 3) return;
        if (g->geom & (GM_CULLFRONT | GM_CULLBACK)) {
            const GspVtx *o = poly[cur];
            float ax = o[0].x / o[0].w, ay = o[0].y / o[0].w, bx = o[1].x / o[1].w, by = o[1].y / o[1].w;
            float cx = o[2].x / o[2].w, cy = o[2].y / o[2].w;
            float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
            if ((area < 0 && (g->geom & GM_CULLBACK)) || (area > 0 && (g->geom & GM_CULLFRONT))) { g->tris_culled++; return; }
        }
        for (i = 1; i + 1 < n; i++) raster(g, &poly[cur][0], &poly[cur][i], &poly[cur][i + 1]);
    }
}

/* ------------------------------------------------------------ etat */

static uint32_t geom_from(int uc, uint32_t m)
{
    uint32_t o = 0;
    if (m & 0x1) o |= GM_ZBUFFER;
    if (m & 0x4) o |= GM_SHADE;
    if (m & 0x10000) o |= GM_FOG;
    if (m & 0x20000) o |= GM_LIGHTING;
    if (m & 0x40000) o |= GM_TEXGEN;
    if (m & 0x80000) o |= GM_TEXGEN_LIN;
    if (uc == UCODE_F3DEX2) {
        if (m & 0x200) o |= GM_CULLFRONT;
        if (m & 0x400) o |= GM_CULLBACK;
        if (m & 0x200000) o |= GM_SMOOTH;
    } else {
        if (m & 0x200) o |= GM_SMOOTH;
        if (m & 0x1000) o |= GM_CULLFRONT;
        if (m & 0x2000) o |= GM_CULLBACK;
    }
    return o;
}

static void set_geom(Gsp *g, uint32_t raw)
{
    g->geom_raw = raw;
    g->geom = geom_from(g->ucode, raw);
}

static void load_light(Gsp *g, int n, uint32_t addr)
{
    uint32_t a = seg_addr(g, addr);
    GspLight *l;
    float x, y, z, len;
    if (n < 0 || n > 9) return;
    l = &g->light[n];
    l->r = g8(g, a); l->g = g8(g, a + 1); l->b = g8(g, a + 2);
    x = (int8_t)g8(g, a + 8); y = (int8_t)g8(g, a + 9); z = (int8_t)g8(g, a + 10);
    len = sqrtf(x * x + y * y + z * z);
    if (len > 1e-6f) { x /= len; y /= len; z /= len; }
    l->x = x; l->y = y; l->z = z;
}

static void load_viewport(Gsp *g, uint32_t addr)
{
    uint32_t a = seg_addr(g, addr);
    g->vp_scale[0] = (int16_t)g16(g, a) / 4.0f;
    g->vp_scale[1] = (int16_t)g16(g, a + 2) / 4.0f;
    g->vp_scale[2] = (int16_t)g16(g, a + 4);
    g->vp_trans[0] = (int16_t)g16(g, a + 8) / 4.0f;
    g->vp_trans[1] = (int16_t)g16(g, a + 10) / 4.0f;
    g->vp_trans[2] = (int16_t)g16(g, a + 12);
}

static void set_othermode(Gsp *g, int high, int shift, int len, uint32_t data)
{
    uint32_t mask, *reg = high ? &g->rdp->omh : &g->rdp->oml;
    if (len <= 0 || shift < 0 || shift + len > 32) return;
    mask = (len == 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
    *reg = (*reg & ~mask) | (data & mask);
}

static void moveword(Gsp *g, int index, uint32_t offset, uint32_t w1)
{
    switch (index) {
    case 0x02:
        if (g->ucode == UCODE_F3DEX2) g->num_lights = (int)(w1 / 24);
        else g->num_lights = (int)((w1 - 0x80000000u) / 32) - 1;
        if (g->num_lights < 0) g->num_lights = 0;
        if (g->num_lights > 8) g->num_lights = 8;
        break;
    case 0x06: g->seg[(offset >> 2) & 15] = w1 & 0xFFFFFF; break;
    case 0x08: g->fog_m = (int16_t)(w1 >> 16); g->fog_o = (int16_t)w1; break;
    default: break;
    }
}

/* ------------------------------------------------------------ boucle principale */

void gsp_run(Gsp *g, int ucode, uint32_t dl_addr)
{
    uint32_t stack[32];
    int sp = 0, guard = 0;
    uint32_t pc = seg_addr(g, dl_addr);
    int ex = ucode != UCODE_F3D;
    g->ucode = ucode;
    set_geom(g, g->geom_raw);
    g->dl_count++;
    while (guard++ < 2000000) {
        uint32_t w0 = g32(g, pc), w1 = g32(g, pc + 4);
        uint32_t op = w0 >> 24;
        pc += 8;
        g->cmds++;
        /* commandes RDP communes */
        if (op >= 0xE4 || (op >= 0xC8 && op <= 0xCF)) {
            if (op == 0xE4 || op == 0xE5) {                  /* rectangle texture : 2 demi-mots suivent */
                uint32_t e0 = g32(g, pc + 4), e1 = g32(g, pc + 12);
                pc += 16;
                rdp_command(g->rdp, w0, w1, e0, e1);
            } else {
                if (op == 0xFD || op == 0xFE || op == 0xFF) w1 = seg_addr(g, w1);
                rdp_command(g->rdp, w0, w1, 0, 0);
            }
            continue;
        }
        if (ucode == UCODE_F3DEX2) {
            switch (op) {
            case 0x00: break;
            case 0x01: { int n = (w0 >> 12) & 0xFF; load_vertices(g, w1, (int)((w0 >> 1) & 0x7F) - n, n); break; }
            case 0x02: break;                                  /* MODIFYVTX */
            case 0x03: break;                                  /* CULLDL */
            case 0x04: pc = seg_addr(g, g->half1); break;      /* BRANCH_Z : toujours pris */
            case 0x05: triangle(g, (int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 8) & 0xFF) / 2, (int)(w0 & 0xFF) / 2); break;
            case 0x06:
            case 0x07:
                triangle(g, (int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 8) & 0xFF) / 2, (int)(w0 & 0xFF) / 2);
                triangle(g, (int)((w1 >> 16) & 0xFF) / 2, (int)((w1 >> 8) & 0xFF) / 2, (int)(w1 & 0xFF) / 2);
                break;
            case 0xD7:
                g->tex_s = (float)(w1 >> 16) / 65536.0f; g->tex_t = (float)(w1 & 0xFFFF) / 65536.0f;
                g->tex_tile = (int)((w0 >> 8) & 7); g->tex_on = (int)((w0 >> 1) & 0x7F) != 0;
                break;
            case 0xD8: pop_mtx(g, (int)(w1 >> 6)); break;
            case 0xD9: set_geom(g, (g->geom_raw & (w0 & 0xFFFFFF)) | w1); break;
            case 0xDA: { int p = (int)(w0 & 0xFF) ^ 1; do_mtx(g, w1, p & 4, p & 2, p & 1); break; }
            case 0xDB: moveword(g, (int)((w0 >> 16) & 0xFF), w0 & 0xFFFF, w1); break;
            case 0xDC: {
                int idx = (int)(w0 & 0xFF), off = (int)((w0 >> 8) & 0xFF) * 8;
                if (idx == 8) load_viewport(g, w1);
                else if (idx == 10) load_light(g, off / 24 - 2, w1);
                break;
            }
            case 0xDD: break;                                  /* LOAD_UCODE */
            case 0xDE:
                if (((w0 >> 16) & 0xFF) == 0 && sp < 32) stack[sp++] = pc;
                pc = seg_addr(g, w1);
                g->dl_count++;
                break;
            case 0xDF: if (sp == 0) return; pc = stack[--sp]; break;
            case 0xE0: break;
            case 0xE1: g->half1 = w1; break;
            case 0xE2: { int len = (int)(w0 & 0xFF) + 1; set_othermode(g, 0, 32 - (int)((w0 >> 8) & 0xFF) - len, len, w1); break; }
            case 0xE3: { int len = (int)(w0 & 0xFF) + 1; set_othermode(g, 1, 32 - (int)((w0 >> 8) & 0xFF) - len, len, w1); break; }
            case 0xF1: g->half2 = w1; break;
            default: g->unknown++; g->last_unknown = w0; break;
            }
        } else {
            switch (op) {
            case 0x00: break;
            case 0x01: { int p = (int)((w0 >> 16) & 0xFF); do_mtx(g, w1, p & 1, p & 2, p & 4); break; }
            case 0x03: {
                int idx = (int)((w0 >> 16) & 0xFF);
                if (idx == 0x80) load_viewport(g, w1);
                else if (idx >= 0x86 && idx <= 0x94) load_light(g, (idx - 0x86) / 2, w1);
                break;
            }
            case 0x04:
                if (ex) load_vertices(g, w1, (int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 10) & 0x3F));
                else load_vertices(g, w1, (int)((w0 >> 16) & 0xF), (int)((w0 >> 20) & 0xF) + 1);
                break;
            case 0x06:
                if (((w0 >> 16) & 0xFF) == 0 && sp < 32) stack[sp++] = pc;
                pc = seg_addr(g, w1);
                g->dl_count++;
                break;
            case 0xB0: if (ex) pc = seg_addr(g, g->half1); break;    /* BRANCH_Z */
            case 0xB1:
                if (ex) {
                    triangle(g, (int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 8) & 0xFF) / 2, (int)(w0 & 0xFF) / 2);
                    triangle(g, (int)((w1 >> 16) & 0xFF) / 2, (int)((w1 >> 8) & 0xFF) / 2, (int)(w1 & 0xFF) / 2);
                }
                break;
            case 0xB2: break;
            case 0xB3: g->half2 = w1; break;
            case 0xB4: g->half1 = w1; break;
            case 0xB5:
                if (ex) {                                       /* QUAD */
                    int a = (int)((w1 >> 24) & 0xFF) / 2, b = (int)((w1 >> 16) & 0xFF) / 2;
                    int c = (int)((w1 >> 8) & 0xFF) / 2, d = (int)(w1 & 0xFF) / 2;
                    triangle(g, a, b, c); triangle(g, a, c, d);
                }
                break;
            case 0xB6: set_geom(g, g->geom_raw & ~w1); break;
            case 0xB7: set_geom(g, g->geom_raw | w1); break;
            case 0xB8: if (sp == 0) return; pc = stack[--sp]; break;
            case 0xB9: set_othermode(g, 0, (int)((w0 >> 8) & 0xFF), (int)(w0 & 0xFF), w1); break;
            case 0xBA: set_othermode(g, 1, (int)((w0 >> 8) & 0xFF), (int)(w0 & 0xFF), w1); break;
            case 0xBB:
                g->tex_s = (float)(w1 >> 16) / 65536.0f; g->tex_t = (float)(w1 & 0xFFFF) / 65536.0f;
                g->tex_tile = (int)((w0 >> 8) & 7); g->tex_on = (int)(w0 & 0xFF) != 0;
                break;
            case 0xBC: moveword(g, (int)(w0 & 0xFF), (w0 >> 8) & 0xFFFF, w1); break;
            case 0xBD: pop_mtx(g, 1); break;
            case 0xBE: break;                                   /* CULLDL */
            case 0xBF: {
                int d = ex ? 2 : 10;
                triangle(g, (int)((w1 >> 16) & 0xFF) / d, (int)((w1 >> 8) & 0xFF) / d, (int)(w1 & 0xFF) / d);
                break;
            }
            case 0xC0: break;
            default: g->unknown++; g->last_unknown = w0; break;
            }
        }
    }
}
