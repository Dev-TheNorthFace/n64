/* n64pad.h : conversion manette PS3 -> manette N64 (format du PIF). */
#ifndef N64_PAD_H
#define N64_PAD_H

#include "common.h"

enum {
    N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000,
    N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100,
    N64_L = 0x0020, N64_R = 0x0010,
    N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001
};

typedef struct {
    int deadzone;   /* % 0..30 */
    int range;      /* amplitude max du stick N64 : 60..85 (vraie manette ~80) */
    int z_on_r2;    /* 0 : Z = L2, 1 : Z = R2 */
    int c_face;     /* 1 : C aussi sur triangle/rond (C-haut/C-droite) */
} PadCfg;

typedef struct {
    uint16_t buttons;
    int8_t x, y;    /* y positif = haut, comme sur la N64 */
} N64Pad;

void n64pad_convert(const Pad *p, const PadCfg *c, N64Pad *out);

#endif
