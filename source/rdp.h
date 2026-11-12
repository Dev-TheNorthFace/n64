/* rdp.h : Reality Display Processor (rendu logiciel).
 * Rasterise directement dans l'image couleur en RDRAM (comme le vrai RDP),
 * avec TMEM 4 Ko, tuiles, formats de texture, combiner, tampon Z, melange. */
#ifndef N64_RDP_H
#define N64_RDP_H

#include <stdint.h>

#define RDP_MAXW 1024
#define RDP_MAXH 1024

typedef struct {
    int fmt, siz, line, tmem, pal, cms, cmt, masks, maskt, shifts, shiftt;
    int uls, ult, lrs, lrt;         /* 10.2 */
} RdpTile;

/* sommet ecran pret a rasteriser */
typedef struct {
    float x, y, z, iw;              /* x,y en pixels, z 0..1, 1/w */
    float r, g, b, a;               /* 0..255 */
    float s, t;                     /* texels (deja multiplies par l'echelle de texture) */
} RdpVtx;

typedef struct {
    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t tmem[4096];
    RdpTile tile[8];
    uint32_t timg_addr; int timg_siz, timg_w;
    uint32_t cimg_addr; int cimg_siz, cimg_w;
    uint32_t zimg_addr;
    uint32_t fill, prim, env, blend, fog;
    uint8_t prim_lod;
    uint16_t prim_z;
    uint32_t omh, oml;
    uint32_t comb_w0, comb_w1;
    int scx0, scy0, scx1, scy1;     /* pixels */
    float *zbuf;
    uint32_t zbuf_for;              /* adresse de l'image Z a laquelle zbuf correspond */
    uint32_t tris, rects, fills, pixels, raw_tris;
    /* etape 5 : cache des tuiles decodees en RGBA 8888 (alloue a part) */
    uint32_t *tcache;
    uint32_t tmem_gen, tc_gen[8], tc_tlut[8];
    int tc_slot[8];
    uint32_t tc_tsig[8];                 /* entree du cache utilisee par chaque tuile (-1 = aucune) */
    uint32_t ce_sig[32], ce_use[32], ce_clock;
    int ce_w[32], ce_h[32];
    int full_sync;
    int fast;                       /* 1 = sans filtrage, 2 = sans filtrage ni perspective */
} Rdp;

int  rdp_init(Rdp *r, uint8_t *rdram, uint32_t size);
void rdp_free(Rdp *r);
/* execute une commande RDP (w0,w1) ; 'extra' = second mot 64 bits (rectangles textures) */
void rdp_command(Rdp *r, uint32_t w0, uint32_t w1, uint32_t ex0, uint32_t ex1);
void rdp_triangle(Rdp *r, const RdpVtx v[3], int textured, int tile, int smooth);
/* longueur en mots de 64 bits d'une commande RDP brute (liste DP directe) */
int  rdp_command_len(uint32_t w0);

#endif
