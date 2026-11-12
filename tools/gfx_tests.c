/* gfx_tests.c : tests du RDP logiciel et du microcode graphique HLE (PC). */
#include "../source/rdp.h"
#include "../source/gsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM (4u << 20)
static uint8_t *ram;
static Rdp rdp;
static Gsp gsp;
static int fails, passes;
#define CHECK(cond, m) do { if (cond) passes++; else { fails++; printf("ECHEC: %s (ligne %d)\n", m, __LINE__); } } while (0)

#define FB   0x100000u     /* image couleur 320x240 16 bits */
#define ZB   0x200000u     /* image Z */
#define TEX  0x300000u
#define DL   0x080000u
#define DATA 0x090000u
#define W 320
#define H 240

static void w16(uint32_t a, uint16_t v) { ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v; }
static void w32(uint32_t a, uint32_t v) { w16(a, (uint16_t)(v >> 16)); w16(a + 2, (uint16_t)v); }
static uint16_t px(int x, int y) { uint32_t a = FB + (uint32_t)(y * W + x) * 2; return (uint16_t)(ram[a] << 8 | ram[a + 1]); }
static int pr(int x, int y) { return (px(x, y) >> 11) & 31; }
static int pg(int x, int y) { return (px(x, y) >> 6) & 31; }
static int pb(int x, int y) { return (px(x, y) >> 1) & 31; }

static void cmd(uint32_t w0, uint32_t w1) { rdp_command(&rdp, w0, w1, 0, 0); }

static uint32_t comb_w0(int a, int c, int Aa, int Ac) { return 0x3C000000u | (uint32_t)a << 20 | (uint32_t)c << 15 | (uint32_t)Aa << 12 | (uint32_t)Ac << 9 | (uint32_t)a << 5 | (uint32_t)c; }
static uint32_t comb_w1(int b, int d, int Ab, int Ad) { return (uint32_t)b << 28 | (uint32_t)b << 24 | (uint32_t)Ab << 21 | 7u << 18 | (uint32_t)d << 15 | (uint32_t)Ab << 12 | (uint32_t)Ad << 9 | (uint32_t)d << 6 | (uint32_t)Ab << 3 | (uint32_t)Ad; }
/* raccourcis : sortie = D (A-B = 0) */
static void comb_out(int d, int ad) { cmd(comb_w0(15, 31, 7, 7), comb_w1(15, d, 7, ad)); }

static void setup(void)
{
    memset(ram, 0, RAM);
    rdp_free(&rdp);
    rdp_init(&rdp, ram, RAM);
    cmd(0x3F000000u | 2u << 19 | (W - 1), FB);                 /* SETCIMG RGBA16 */
    cmd(0x3E000000u, ZB);
    cmd(0x2D000000u, (uint32_t)(W << 2) << 12 | (H << 2));     /* scissor */
}

static void clear(uint16_t c)
{
    cmd(0x2F000000u | 3u << 20, 0);                            /* mode FILL */
    cmd(0x37000000u, (uint32_t)c << 16 | c);
    cmd(0x36000000u | (uint32_t)((W - 1) << 2) << 12 | ((H - 1) << 2), 0);
}

static void save_ppm(const char *name)
{
    FILE *f = fopen(name, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint8_t c[3] = { (uint8_t)(pr(x, y) << 3), (uint8_t)(pg(x, y) << 3), (uint8_t)(pb(x, y) << 3) };
            fwrite(c, 1, 3, f);
        }
    fclose(f);
}

static RdpVtx V(float x, float y, float z, int r, int g, int b, int a)
{
    RdpVtx v;
    memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.z = z; v.iw = 1; v.r = (float)r; v.g = (float)g; v.b = (float)b; v.a = (float)a;
    return v;
}

static void test_fill(void)
{
    setup();
    clear(0xF801);                                           /* rouge */
    CHECK(px(0, 0) == 0xF801 && px(319, 239) == 0xF801 && px(160, 120) == 0xF801, "remplissage plein ecran");
    cmd(0x37000000u, 0x07C107C1u);
    cmd(0x36000000u | (uint32_t)(20 << 2) << 12 | (20 << 2), (uint32_t)(10 << 2) << 12 | (10 << 2));
    CHECK(px(10, 10) == 0x07C1 && px(20, 20) == 0x07C1 && px(21, 21) == 0xF801 && px(9, 10) == 0xF801, "rectangle de remplissage (bornes inclusives)");
    CHECK(rdp.fills == 2, "compteur de remplissages");
}

static void test_triangle(void)
{
    RdpVtx v[3];
    setup();
    clear(0x0001);
    cmd(0x2F000000u, 0);                                     /* 1 cycle */
    comb_out(4, 4);                                          /* sortie = couleur des sommets */
    v[0] = V(50, 50, 0.5f, 255, 0, 0, 255); v[1] = V(250, 60, 0.5f, 0, 255, 0, 255); v[2] = V(100, 200, 0.5f, 0, 0, 255, 255);
    rdp_triangle(&rdp, v, 0, 0, 1);
    CHECK(px(5, 5) == 0x0001, "hors du triangle intact");
    CHECK(pr(52, 52) > 25 && pg(52, 52) < 6, "degrade : coin rouge");
    CHECK(pg(240, 60) > 25, "degrade : coin vert");
    CHECK(pb(102, 195) > 25, "degrade : coin bleu");
    CHECK(rdp.tris == 1 && rdp.pixels > 10000, "triangle rasterise");
    rdp_triangle(&rdp, v, 0, 0, 0);
    CHECK(pr(102, 195) > 25 && pb(102, 195) == 0, "ombrage plat = premier sommet");
    save_ppm("shots/gfx_triangle.ppm");
}

static void test_zbuffer(void)
{
    RdpVtx a[3], b[3];
    setup();
    clear(0x0001);
    cmd(0x2F000000u, 0x30);                                  /* z_cmp + z_upd */
    comb_out(4, 4);
    a[0] = V(20, 20, 0.3f, 255, 0, 0, 255); a[1] = V(200, 20, 0.3f, 255, 0, 0, 255); a[2] = V(20, 200, 0.3f, 255, 0, 0, 255);
    b[0] = V(40, 40, 0.6f, 0, 255, 0, 255); b[1] = V(220, 40, 0.6f, 0, 255, 0, 255); b[2] = V(40, 220, 0.6f, 0, 255, 0, 255);
    rdp_triangle(&rdp, a, 0, 0, 0);
    rdp_triangle(&rdp, b, 0, 0, 0);                          /* plus loin : cache */
    CHECK(pr(60, 60) == 31 && pg(60, 60) == 0, "tampon Z : le proche reste devant");
    CHECK(pg(190, 50) == 31, "tampon Z : le loin visible hors recouvrement");
    /* effacement du Z via un remplissage de l'image Z */
    cmd(0x3F000000u | 2u << 19 | (W - 1), ZB);
    clear(0xFFFC);
    cmd(0x3F000000u | 2u << 19 | (W - 1), FB);
    cmd(0x2F000000u, 0x30);
    rdp_triangle(&rdp, b, 0, 0, 0);
    CHECK(pg(60, 60) == 31, "Z efface : le triangle passe");
}

static void load_tex_rgba16(int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint16_t c = (uint16_t)(((x * 31 / (w - 1)) << 11) | ((y * 31 / (h - 1)) << 6) | 1);
            w16(TEX + (uint32_t)(y * w + x) * 2, c);
        }
    cmd(0x3D000000u | 2u << 19 | (uint32_t)(w - 1), TEX);                       /* SETTIMG */
    cmd(0x35000000u | 2u << 19, 7u << 24);                                      /* SETTILE 7 (chargement) */
    {
        uint32_t words = (uint32_t)w * 2 / 8, dxt = (2048 + words - 1) / words;
        cmd(0x33000000u, 7u << 24 | (uint32_t)(w * h - 1) << 12 | dxt);          /* LOADBLOCK */
    }
    cmd(0x35000000u | 2u << 19 | (uint32_t)(w * 2 / 8) << 9, 0);                /* SETTILE 0 */
    cmd(0x32000000u, (uint32_t)((w - 1) << 2) << 12 | ((h - 1) << 2));          /* SETTILESIZE */
}

static void test_texrect_copy(void)
{
    int ok = 1, x, y;
    setup();
    clear(0x0001);
    load_tex_rgba16(16, 16);
    cmd(0x2F000000u | 2u << 20, 0);                                             /* COPY */
    rdp_command(&rdp, 0xE4000000u | (uint32_t)((100 + 15) << 2) << 12 | ((50 + 15) << 2),
                (uint32_t)(100 << 2) << 12 | (50 << 2), 0, 4u << 10 << 16 | 1u << 10);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            uint16_t want = (uint16_t)(((x * 31 / 15) << 11) | ((y * 31 / 15) << 6) | 1);
            if (px(100 + x, 50 + y) != want) ok = 0;
        }
    CHECK(ok, "rectangle copie : texels exacts (lignes impaires entrelacees comprises)");
    CHECK(px(99, 50) == 0x0001 && px(116, 50) == 0x0001, "rectangle copie : bornes");
    /* 1 cycle, etire x2, combinaison TEXEL0 */
    cmd(0x2F000000u, 0);
    cmd(comb_w0(15, 31, 7, 7), comb_w1(15, 1, 7, 1));
    rdp_command(&rdp, 0xE4000000u | (uint32_t)(232 << 2) << 12 | (82 << 2), (uint32_t)(200 << 2) << 12 | (50 << 2), 0, 512u << 16 | 512u);
    CHECK(px(200, 50) == px(201, 51) && px(230, 80) == ((31 << 11) | (31 << 6) | 1), "rectangle 1 cycle agrandi x2");
}

static void test_ci4(void)
{
    int i;
    setup();
    clear(0x0001);
    for (i = 0; i < 16; i++) w16(TEX + 0x1000 + (uint32_t)i * 2, (uint16_t)(i == 3 ? 0x07C1 : 0xF801));   /* palette */
    for (i = 0; i < 64; i++) ram[TEX + (uint32_t)i] = 0x33;      /* CI4, lignes de 8 octets */
    cmd(0x3D000000u | 2u << 19, TEX + 0x1000);
    cmd(0x35000000u | (256u << 0), 7u << 24);                               /* tile 7 a TMEM 0x800 */
    rdp_command(&rdp, 0x30000000u, 7u << 24 | (15u << 2) << 12, 0, 0);     /* LOADTLUT 16 entrees */
    cmd(0x3D000000u | 1u << 19 | 7, TEX);                                   /* image 8 bits (CI4 chargee en 8 bits) */
    cmd(0x35000000u | 1u << 19, 7u << 24);
    cmd(0x33000000u, 7u << 24 | (63u << 12) | 2048);
    cmd(0x35000000u | 2u << 21 | 0u << 19 | (1u << 9), 0);                  /* tile 0 : CI4, ligne 8 octets */
    cmd(0x32000000u, (7u << 2) << 12 | (7u << 2));
    cmd(0x2F000000u | 2u << 20 | 0x8000, 0);                                /* COPY + TLUT RGBA16 */
    rdp_command(&rdp, 0xE4000000u | (uint32_t)(17 << 2) << 12 | (17 << 2), (uint32_t)(10 << 2) << 12 | (10 << 2), 0, 4u << 10 << 16 | 1u << 10);
    if (px(10, 10) != 0x07C1) printf("  ci4: %04X %04X tmem800=%02X%02X\n", px(10, 10), px(17, 17), rdp.tmem[0x800 + 24], rdp.tmem[0x801 + 24]);
    CHECK(px(10, 10) == 0x07C1 && px(17, 17) == 0x07C1, "texture CI4 + palette");
}

/* deux triangles qui partagent une arete oblique : aucun trou, aucun doublon */
static void test_shared_edge(void)
{
    RdpVtx a[3], b[3];
    int x, y, holes = 0;
    setup();
    clear(0x0001);
    cmd(0x2F000000u, 0);
    comb_out(4, 4);
    a[0] = V(10.3f, 10.7f, 0, 255, 0, 0, 255); a[1] = V(300.1f, 37.9f, 0, 255, 0, 0, 255); a[2] = V(23.6f, 229.2f, 0, 255, 0, 0, 255);
    b[0] = V(300.1f, 37.9f, 0, 0, 255, 0, 255); b[1] = V(310.4f, 220.3f, 0, 0, 255, 0, 255); b[2] = V(23.6f, 229.2f, 0, 0, 255, 0, 255);
    rdp_triangle(&rdp, a, 0, 0, 0);
    rdp_triangle(&rdp, b, 0, 0, 0);
    /* le long de l'arete commune, chaque pixel doit etre rouge ou vert */
    for (y = 45; y < 220; y++)
        for (x = 30; x < 290; x++) {
            float t = (y - 37.9f) / (229.2f - 37.9f), ex = 300.1f + (23.6f - 300.1f) * t;
            if (x > ex - 3 && x < ex + 3 && px(x, y) == 0x0001) holes++;
        }
    CHECK(holes == 0, "arete commune : aucun trou entre deux triangles");
}

static void test_blend(void)
{
    RdpVtx v[3];
    setup();
    clear(0xF801);                                           /* fond rouge */
    /* 1 cycle, melange CLR_IN*A + MEM*(1-A), force_blend */
    cmd(0x2F000000u, 0x00404000u | 0x4000);
    comb_out(4, 4);
    v[0] = V(0, 0, 0, 0, 0, 255, 128); v[1] = V(300, 0, 0, 0, 0, 255, 128); v[2] = V(0, 200, 0, 0, 0, 255, 128);
    rdp_triangle(&rdp, v, 0, 0, 0);
    CHECK(pr(20, 20) >= 14 && pr(20, 20) <= 17 && pb(20, 20) >= 14 && pb(20, 20) <= 17, "transparence 50 %");
}

static void test_combiner_prim(void)
{
    RdpVtx v[3];
    setup();
    clear(0x0001);
    cmd(0x2F000000u, 0);
    cmd(0x3A000000u, 0x00FF00FFu);                           /* prim vert */
    /* (PRIM - 0) * SHADE + 0 */
    cmd(comb_w0(3, 4, 3, 4), comb_w1(15, 7, 7, 7));
    v[0] = V(10, 10, 0, 128, 128, 128, 255); v[1] = V(100, 10, 0, 128, 128, 128, 255); v[2] = V(10, 100, 0, 128, 128, 128, 255);
    rdp_triangle(&rdp, v, 0, 0, 0);
    CHECK(pg(20, 20) >= 15 && pg(20, 20) <= 16 && pr(20, 20) == 0, "combiner : PRIM * SHADE");
}

/* ------------------------------------------------------------ microcode */

static uint32_t dlp;
static void dl(uint32_t w0, uint32_t w1) { w32(dlp, w0); w32(dlp + 4, w1); dlp += 8; }

static void put_identity(uint32_t a, float sx, float sy)
{
    int i;
    float m[16] = { 0 };
    m[0] = sx; m[5] = sy; m[10] = 1; m[15] = 1;
    for (i = 0; i < 16; i++) {
        int32_t f = (int32_t)(m[i] * 65536.0f);
        w16(a + (uint32_t)i * 2, (uint16_t)(f >> 16));
        w16(a + 32 + (uint32_t)i * 2, (uint16_t)(f & 0xFFFF));
    }
}

static void put_vtx(uint32_t a, int x, int y, int z, uint32_t rgba)
{
    w16(a, (uint16_t)x); w16(a + 2, (uint16_t)y); w16(a + 4, (uint16_t)z); w16(a + 6, 0);
    w16(a + 8, 0); w16(a + 10, 0); w32(a + 12, rgba);
}

static void common_scene(void)
{
    /* matrice identite (coordonnees en "unites ecran" divisees par 160/120 via la projection) */
    put_identity(DATA, 1.0f / 160, 1.0f / 120);           /* projection */
    put_identity(DATA + 64, 1, 1);                        /* modelview */
    w16(DATA + 128, 160 * 4); w16(DATA + 130, 120 * 4); w16(DATA + 132, 511); w16(DATA + 134, 0);   /* viewport */
    w16(DATA + 136, 160 * 4); w16(DATA + 138, 120 * 4); w16(DATA + 140, 511); w16(DATA + 142, 0);
    /* triangle direct (sens trigo en y haut) et triangle inverse */
    put_vtx(DATA + 0x100, -100, -80, 0, 0xFF0000FFu);
    put_vtx(DATA + 0x110, -20, -80, 0, 0xFF0000FFu);
    put_vtx(DATA + 0x120, -100, 0, 0, 0xFF0000FFu);
    put_vtx(DATA + 0x130, 100, 80, 0, 0x00FF00FFu);
    put_vtx(DATA + 0x140, 20, 80, 0, 0x00FF00FFu);       /* sens horaire = face arriere */
    put_vtx(DATA + 0x150, 100, 0, 0, 0x00FF00FFu);
}

static void test_f3dex2(void)
{
    setup();
    clear(0x0001);
    gsp_init(&gsp, ram, RAM, &rdp);
    common_scene();
    dlp = DL;
    dl(0xE3000A01u, 0);                                    /* cycle type 1 cycle (shift 20, len 2) */
    dl(0xFC000000u | (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u,
       (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (4u << 15) | (7u << 12) | (4u << 9) | (4u << 6) | (7u << 3) | 4u);
    dl(0xDA380007u, DATA);                                 /* projection, load */
    dl(0xDA380003u, DATA + 64);                            /* modelview, load */
    dl(0xDC080008u, DATA + 128);                           /* viewport */
    dl(0xD9000000u, 0x00200004u | 0x400);                  /* shade, smooth, cull back */
    dl(0x01006000u | (6u << 12) | (6u << 1), DATA + 0x100);/* 6 sommets a 0 */
    dl(0x05000204u, 0);                                    /* tri 0,1,2 */
    dl(0x050A0806u, 0);                                    /* tri 5,4,3 (face arriere) */
    dl(0xDF000000u, 0);
    gsp_run(&gsp, UCODE_F3DEX2, DL);
    CHECK(gsp.tris_in == 2, "F3DEX2 : 2 triangles recus");
    CHECK(gsp.tris_culled == 1, "F3DEX2 : face arriere eliminee");
    CHECK(pr(70, 170) == 31, "F3DEX2 : triangle rouge a l'ecran (bas gauche)");
    CHECK(pg(240, 60) == 0, "F3DEX2 : triangle vert elimine");
    CHECK(gsp.unknown == 0, "F3DEX2 : aucune commande inconnue");
    /* sans elimination : le vert apparait */
    dlp = DL + 5 * 8;
    dl(0xD9000000u, 0x00200004u);
    gsp_run(&gsp, UCODE_F3DEX2, DL);
    CHECK(pg(240, 60) == 31, "F3DEX2 : triangle vert sans elimination");
    save_ppm("shots/gfx_f3dex2.ppm");
}

static void test_f3d(void)
{
    setup();
    clear(0x0001);
    gsp_init(&gsp, ram, RAM, &rdp);
    common_scene();
    dlp = DL;
    dl(0xBA001402u, 0);                                    /* SETOTHERMODE_H : cycle (shift 20, len 2) = 1 cycle */
    dl(0xFC000000u | (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u,
       (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (4u << 15) | (7u << 12) | (4u << 9) | (4u << 6) | (7u << 3) | 4u);
    dl(0x01030040u, DATA);                                 /* projection + load */
    dl(0x01020040u, DATA + 64);                            /* modelview load */
    dl(0x03800010u, DATA + 128);                           /* viewport */
    dl(0xB7000000u, 0x00000204u);                          /* shade + smooth */
    /* sous-liste via segment 6 */
    dl(0xBC000606u, 0x00000000u);                          /* segment 6 = 0 */
    dl(0x06000000u, 0x06000000u | (DL + 0x400));
    dl(0xB8000000u, 0);
    dlp = DL + 0x400;
    dl(0x04500060u, DATA + 0x100);                         /* F3D : 6 sommets (n-1=5) a 0 */
    dl(0xBF000000u, (0u << 16) | (10u << 8) | 20u);        /* indices * 10 */
    dl(0xBF000000u, (30u << 16) | (40u << 8) | 50u);
    dl(0xB8000000u, 0);
    gsp_run(&gsp, UCODE_F3D, DL);
    CHECK(gsp.tris_in == 2 && gsp.tris_culled == 0, "F3D : 2 triangles, sous-liste par segment");
    CHECK(pr(70, 170) == 31 && pg(240, 60) == 31, "F3D : les deux triangles dessines");
}

static void test_f3dex_quad_clip(void)
{
    setup();
    clear(0x0001);
    gsp_init(&gsp, ram, RAM, &rdp);
    common_scene();
    /* projection perspective simple : w = -z */
    {
        int i;
        float m[16] = { 1.0f / 160, 0, 0, 0,  0, 1.0f / 120, 0, 0,  0, 0, -1, -1,  0, 0, -0.1f, 0 };
        for (i = 0; i < 16; i++) {
            int32_t f = (int32_t)(m[i] * 65536.0f);
            w16(DATA + 0x200 + (uint32_t)i * 2, (uint16_t)(f >> 16));
            w16(DATA + 0x220 + (uint32_t)i * 2, (uint16_t)(f & 0xFFFF));
        }
    }
    /* quad sol qui traverse le plan proche (z de -2 a +1) */
    put_vtx(DATA + 0x300, -100, -50, 1, 0xFFFF00FFu);
    put_vtx(DATA + 0x310, 100, -50, 1, 0xFFFF00FFu);
    put_vtx(DATA + 0x320, 100, -50, -2, 0xFFFF00FFu);
    put_vtx(DATA + 0x330, -100, -50, -2, 0xFFFF00FFu);
    dlp = DL;
    dl(0xBA001402u, 0);
    dl(0xFC000000u | (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u,
       (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (4u << 15) | (7u << 12) | (4u << 9) | (4u << 6) | (7u << 3) | 4u);
    dl(0x01030040u, DATA + 0x200);
    dl(0x01020040u, DATA + 64);
    dl(0x03800010u, DATA + 128);
    dl(0xB7000000u, 0x00000004u);
    dl(0x04001000u | (4u << 10), DATA + 0x300);            /* F3DEX : 4 sommets a 0 */
    dl(0xB5000000u, 0x00020406u);                          /* QUAD 0,1,2,3 */
    dl(0xB8000000u, 0);
    gsp_run(&gsp, UCODE_F3DEX, DL);
    CHECK(gsp.tris_in == 2, "F3DEX : quad = 2 triangles");
    CHECK(gsp.tris_clipped >= 1, "F3DEX : decoupage plan proche");
    CHECK(pr(160, 230) == 31 && pg(160, 230) == 31, "F3DEX : sol visible devant la camera");
    CHECK(px(160, 10) == 0x0001, "F3DEX : ciel intact");
}

static void test_detect(void)
{
    memset(ram, 0, 0x10000);
    memcpy(ram + 0x1000, "RSP Gfx ucode F3DZEX.NoN fifo 2.06H Yoshitaka Yasumoto 1998", 59);
    CHECK(gsp_detect_ucode(ram, RAM, 0x80001000u, 0x800) == UCODE_F3DEX2, "detection F3DZEX (Zelda)");
    memset(ram + 0x1000, 0, 0x800);
    memcpy(ram + 0x1000, "RSP Gfx ucode F3DEX       0.95 Yoshitaka Yasumoto Nintendo.", 59);
    CHECK(gsp_detect_ucode(ram, RAM, 0x80001000u, 0x800) == UCODE_F3DEX, "detection F3DEX (Mario Kart)");
    memset(ram + 0x1000, 0, 0x800);
    memcpy(ram + 0x1000, "RSP SW Version: 2.0D, 04-01-96", 30);
    CHECK(gsp_detect_ucode(ram, RAM, 0x80001000u, 0x800) == UCODE_F3D, "detection F3D (Mario 64)");
}

static void test_fuzz(void)
{
    int i;
    uint32_t seed = 12345;
    setup();
    gsp_init(&gsp, ram, RAM, &rdp);
    for (i = 0; i < 0x4000; i++) { seed = seed * 1103515245u + 12345u; ram[DL + (uint32_t)i] = (uint8_t)(seed >> 16); }
    w32(DL + 0x3FF8, 0xDF000000u);
    gsp_run(&gsp, UCODE_F3DEX2, DL);
    gsp_run(&gsp, UCODE_F3D, DL);
    gsp_run(&gsp, UCODE_F3DEX, DL);
    CHECK(1, "listes aleatoires sans plantage");
}

int main(void)
{
    ram = (uint8_t *)calloc(RAM, 1);
    rdp_init(&rdp, ram, RAM);
    test_fill();
    test_triangle();
    test_zbuffer();
    test_texrect_copy();
    test_ci4();
    test_shared_edge();
    test_blend();
    test_combiner_prim();
    test_f3dex2();
    test_f3d();
    test_f3dex_quad_clip();
    test_detect();
    test_fuzz();
    rdp_free(&rdp);
    free(ram);
    printf("Graphismes : %d reussis, %d echecs\n", passes, fails);
    return fails ? 1 : 0;
}
