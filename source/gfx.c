/* gfx.c */
#include "gfx.h"
#include "roms.h"
#include <string.h>

const uint8_t *font_glyph(int ch);

static Surface *S;
static float sc, ox, oy;

void gfx_begin(Surface *s)
{
    float sx = (float)s->w / REF_W, sy = (float)s->h / REF_H;
    S = s;
    sc = sx < sy ? sx : sy;
    ox = (s->w - REF_W * sc) * 0.5f;
    oy = (s->h - REF_H * sc) * 0.5f;
}

void gfx_clear(uint32_t color)
{
    int y, x;
    for (y = 0; y < S->h; y++) {
        uint32_t *row = S->px + (size_t)y * S->pitch;
        for (x = 0; x < S->w; x++) row[x] = color;
    }
}

static int to_px(float v, float o) { return (int)(v * sc + o + 0.5f); }

static int clip(int *x0, int *y0, int *x1, int *y1)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > S->w) *x1 = S->w;
    if (*y1 > S->h) *y1 = S->h;
    return *x0 < *x1 && *y0 < *y1;
}

void gfx_rect(float x, float y, float w, float h, uint32_t color)
{
    int x0 = to_px(x, ox), y0 = to_px(y, oy), x1 = to_px(x + w, ox), y1 = to_px(y + h, oy), i, j;
    if (x1 == x0 && w > 0) x1++;
    if (y1 == y0 && h > 0) y1++;
    if (!clip(&x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) {
        uint32_t *row = S->px + (size_t)j * S->pitch;
        for (i = x0; i < x1; i++) row[i] = color;
    }
}

static uint32_t blend(uint32_t a, uint32_t b, int t)
{
    uint32_t rb = ((a & 0xFF00FF) * (256 - t) + (b & 0xFF00FF) * t) >> 8;
    uint32_t g = ((a & 0x00FF00) * (256 - t) + (b & 0x00FF00) * t) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

void gfx_rect_blend(float x, float y, float w, float h, uint32_t color, int alpha)
{
    int x0 = to_px(x, ox), y0 = to_px(y, oy), x1 = to_px(x + w, ox), y1 = to_px(y + h, oy), i, j;
    if (!clip(&x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) {
        uint32_t *row = S->px + (size_t)j * S->pitch;
        for (i = x0; i < x1; i++) row[i] = blend(row[i], color, alpha);
    }
}

void gfx_line(float x0, float y0, float x1, float y1, float t, uint32_t color)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = dx < 0 ? -dx : dx, ly = dy < 0 ? -dy : dy;
    int n, i;
    if (ly > len) len = ly;
    n = (int)(len / (t * 0.5f)) + 1;
    for (i = 0; i <= n; i++) {
        float f = (float)i / n;
        gfx_rect(x0 + dx * f - t / 2, y0 + dy * f - t / 2, t, t, color);
    }
}

void gfx_frame(float x, float y, float w, float h, float t, uint32_t color)
{
    gfx_rect(x, y, w, t, color);
    gfx_rect(x, y + h - t, w, t, color);
    gfx_rect(x, y, t, h, color);
    gfx_rect(x + w - t, y, t, h, color);
}

float gfx_text_width(const char *s, float size)
{
    char buf[512];
    text_fold(s, buf, sizeof buf);
    return strlen(buf) * 6 * size - (buf[0] ? size : 0);
}

float gfx_text(float x, float y, const char *s, float size, uint32_t color)
{
    char buf[512];
    const char *p;
    float cx = x;
    text_fold(s, buf, sizeof buf);
    for (p = buf; *p; p++) {
        if (*p != ' ') {
            const uint8_t *g = font_glyph((unsigned char)*p);
            int r, c;
            for (r = 0; r < 7; r++) {
                int c0 = -1;
                for (c = 0; c <= 5; c++) {        /* regroupe les pixels contigus */
                    int on = c < 5 && (g[r] & (1 << (4 - c)));
                    if (on && c0 < 0) c0 = c;
                    if (!on && c0 >= 0) {
                        gfx_rect(cx + c0 * size, y + r * size, (c - c0) * size, size, color);
                        c0 = -1;
                    }
                }
            }
        }
        cx += 6 * size;
    }
    return cx - x;
}

int gfx_wrap(float x, float y, float w, const char *s, float size, uint32_t color, int maxlines)
{
    char buf[512], line[128];
    const char *p;
    int maxc = (int)(w / (6 * size)), lines = 0;
    text_fold(s, buf, sizeof buf);
    p = buf;
    if (maxc < 4) maxc = 4;
    if (maxc > 127) maxc = 127;
    while (*p && lines < maxlines) {
        int n = (int)strlen(p), cut = n;
        while (*p == ' ') p++;
        n = (int)strlen(p);
        if (n > maxc) {
            cut = maxc;
            while (cut > 0 && p[cut] != ' ') cut--;
            if (cut == 0) cut = maxc;
        } else cut = n;
        memcpy(line, p, cut);
        line[cut] = 0;
        if (lines == maxlines - 1 && (int)strlen(p) > cut && cut > 3) strcpy(line + cut - 3, "...");
        gfx_text(x, y + lines * 10 * size, line, size, color);
        p += cut;
        lines++;
    }
    return lines;
}

void gfx_blit_scaled(const uint32_t *src, int lw, int lh, float x, float y, float w, float h, int scanlines)
{
    int x0 = to_px(x, ox), y0 = to_px(y, oy), x1 = to_px(x + w, ox), y1 = to_px(y + h, oy);
    int dw = x1 - x0, dh = y1 - y0, i, j;
    static int xmap[4096];
    int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
    if (dw <= 0 || dh <= 0 || dw > 4096) return;
    if (!clip(&cx0, &cy0, &cx1, &cy1)) return;
    for (i = cx0; i < cx1; i++) xmap[i - x0] = (i - x0) * lw / dw;
    for (j = cy0; j < cy1; j++) {
        int sy = (j - y0) * lh / dh;
        const uint32_t *srow = src + (size_t)sy * lw;
        uint32_t *row = S->px + (size_t)j * S->pitch;
        /* scanlines : assombrit la derniere ligne ecran de chaque ligne logique */
        int dark = scanlines && dh >= lh * 2 && ((j - y0 + 1) * lh / dh) != sy;
        if (dark) {
            for (i = cx0; i < cx1; i++) {
                uint32_t c = srow[xmap[i - x0]];
                row[i] = (c >> 1) & 0x7F7F7F;
            }
        } else {
            for (i = cx0; i < cx1; i++) row[i] = srow[xmap[i - x0]];
        }
    }
}

void gfx_rect_to_px(float x, float y, float w, float h, int r[4])
{
    int x0 = to_px(x, ox), y0 = to_px(y, oy), x1 = to_px(x + w, ox), y1 = to_px(y + h, oy);
    if (!clip(&x0, &y0, &x1, &y1)) { r[0] = r[1] = r[2] = r[3] = 0; return; }
    r[0] = x0; r[1] = y0; r[2] = x1 - x0; r[3] = y1 - y0;
}
