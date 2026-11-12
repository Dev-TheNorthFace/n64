/* gsp.h : emulation haut niveau (HLE) du microcode graphique du RSP.
 * Interprete les listes d'affichage F3D (Mario 64), F3DEX (Mario Kart 64)
 * et F3DEX2 / F3DZEX (Zelda Ocarina of Time) et envoie triangles et
 * commandes au RDP logiciel. */
#ifndef N64_GSP_H
#define N64_GSP_H

#include <stdint.h>
#include "rdp.h"

enum { UCODE_F3D = 0, UCODE_F3DEX = 1, UCODE_F3DEX2 = 2 };

typedef struct {
    float x, y, z, w;             /* espace de decoupage (clip) */
    float r, g, b, a;
    float s, t;
} GspVtx;

typedef struct { float r, g, b, x, y, z; } GspLight;

#define GSP_MTX_DEPTH 32

typedef struct {
    uint8_t *rdram;
    uint32_t rdram_size;
    Rdp *rdp;
    int ucode;
    uint32_t seg[16];
    float proj[4][4];
    float mv[GSP_MTX_DEPTH][4][4];
    int mv_top;
    float mvp[4][4];
    int mvp_dirty;
    GspVtx v[80];
    float vp_scale[3], vp_trans[3];
    GspLight light[10];
    int num_lights;
    uint32_t geom;                /* drapeaux internes GM_* */
    uint32_t geom_raw;            /* valeur brute du microcode */
    float tex_s, tex_t;           /* echelles de texture */
    int tex_tile, tex_on;
    uint32_t half1, half2;
    int16_t fog_m, fog_o;
    /* statistiques */
    uint32_t cmds, tris_in, tris_culled, tris_clipped, dl_count, unknown;
    uint32_t last_unknown;
} Gsp;

/* drapeaux de geometrie internes (independants du microcode) */
#define GM_ZBUFFER   0x01
#define GM_SHADE     0x02
#define GM_SMOOTH    0x04
#define GM_CULLFRONT 0x08
#define GM_CULLBACK  0x10
#define GM_FOG       0x20
#define GM_LIGHTING  0x40
#define GM_TEXGEN    0x80
#define GM_TEXGEN_LIN 0x100

void gsp_init(Gsp *g, uint8_t *rdram, uint32_t size, Rdp *rdp);
/* reconnait le microcode a partir de son segment de donnees */
int  gsp_detect_ucode(const uint8_t *rdram, uint32_t size, uint32_t data_addr, uint32_t data_size);
/* execute une liste d'affichage complete */
void gsp_run(Gsp *g, int ucode, uint32_t dl_addr);
const char *gsp_ucode_name(int u);

#endif
