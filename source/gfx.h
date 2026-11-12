/* gfx.h : dessin logiciel dans le framebuffer, en coordonnees de reference
 * 1280x720 mises a l'echelle selon la resolution reelle (480p a 1080p). */
#ifndef ODY_GFX_H
#define ODY_GFX_H

#include "common.h"

#define REF_W 1280
#define REF_H 720

void gfx_begin(Surface *s);
void gfx_clear(uint32_t color);
void gfx_rect(float x, float y, float w, float h, uint32_t color);
void gfx_rect_blend(float x, float y, float w, float h, uint32_t color, int alpha /*0-255*/);
void gfx_line(float x0, float y0, float x1, float y1, float t, uint32_t color);
void gfx_frame(float x, float y, float w, float h, float t, uint32_t color);
float gfx_text(float x, float y, const char *s, float size, uint32_t color);
float gfx_text_width(const char *s, float size);
int  gfx_wrap(float x, float y, float w, const char *s, float size, uint32_t color, int maxlines);
/* copie une image logique (lw x lh) dans un rectangle de reference */
void gfx_blit_scaled(const uint32_t *src, int lw, int lh, float x, float y, float w, float h, int scanlines);

/* rectangle de reference -> pixels de l'ecran (x, y, largeur, hauteur), borne a l'ecran */
void gfx_rect_to_px(float x, float y, float w, float h, int r[4]);

#endif
