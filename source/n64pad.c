/* n64pad.c */
#include "n64pad.h"
#include <math.h>

void n64pad_convert(const Pad *p, const PadCfg *c, N64Pad *o)
{
    float x = p->lx, y = -p->ly, mag, dz = c->deadzone / 100.0f;
    uint16_t b = 0;

    /* zone morte radiale puis remise a l'echelle */
    mag = sqrtf(x * x + y * y);
    if (mag <= dz) { x = y = 0; }
    else {
        float s = (mag - dz) / (1.0f - dz) / mag;
        if (mag > 1.0f) s = 1.0f / mag;          /* le stick PS3 depasse un peu le cercle */
        x *= s; y *= s;
    }
    o->x = (int8_t)lroundf(x * c->range);
    o->y = (int8_t)lroundf(y * c->range);

    if (p->held & B_CROSS) b |= N64_A;
    if (p->held & B_SQUARE) b |= N64_B;
    if (p->held & (c->z_on_r2 ? B_R2 : B_L2)) b |= N64_Z;
    if (p->held & B_START) b |= N64_START;
    if (p->held & B_UP) b |= N64_DU;
    if (p->held & B_DOWN) b |= N64_DD;
    if (p->held & B_LEFT) b |= N64_DL;
    if (p->held & B_RIGHT) b |= N64_DR;
    if (p->held & B_L1) b |= N64_L;
    if (p->held & B_R1) b |= N64_R;
    if (p->ry < -0.5f) b |= N64_CU;
    if (p->ry > 0.5f) b |= N64_CD;
    if (p->rx < -0.5f) b |= N64_CL;
    if (p->rx > 0.5f) b |= N64_CR;
    if (c->c_face) {
        if (p->held & B_TRIANGLE) b |= N64_CU;
        if (p->held & B_CIRCLE) b |= N64_CR;
    }
    o->buttons = b;
}
