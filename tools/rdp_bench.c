/* rdp_bench.c : mesure du RDP logiciel sur une scene type "jeu" :
 * 1500 triangles textures (bilineaire, perspective), tampon Z, ~3 couches. */
#include "../source/rdp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint8_t *ram;
static Rdp r;
static void cmd(uint32_t w0, uint32_t w1) { rdp_command(&r, w0, w1, 0, 0); }

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 30, f, i, cycle2 = argc > 2;
    uint32_t seed = 1;
    double t0, t;
    ram = calloc(4 << 20, 1);
    rdp_init(&r, ram, 4 << 20);
    for (i = 0; i < 32 * 32; i++) { ram[0x300000 + i * 2] = (uint8_t)(i * 7); ram[0x300001 + i * 2] = (uint8_t)(i | 1); }
    cmd(0x3F000000u | 2u << 19 | 319, 0x100000);
    cmd(0x3E000000u, 0x200000);
    cmd(0x2D000000u, (320u << 2) << 12 | (240u << 2));
    cmd(0x3D000000u | 2u << 19 | 31, 0x300000);
    cmd(0x35000000u | 2u << 19, 7u << 24);
    cmd(0x33000000u, 7u << 24 | (1023u << 12) | 256);
    cmd(0x35000000u | 2u << 19 | 8u << 9, 0x00014050);
    cmd(0x32000000u, 0x0007C07C);
    /* TEXEL0 * SHADE */
    cmd(0x3C000000u | 1u << 20 | 4u << 15 | 7u << 12 | 7u << 9 | 1u << 5 | 4u,
        15u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 7u << 15 | 7u << 12 | 4u << 9 | 7u << 6 | 7u << 3 | 4u);
    t0 = now();
    for (f = 0; f < frames; f++) {
        cmd(0x2F000000u | 3u << 20, 0);
        cmd(0x37000000u, 0x00010001);
        cmd(0x36000000u | (319u << 2) << 12 | (239u << 2), 0);
        cmd(0x2F000000u | (cycle2 ? 1u : 0u) << 20 | 0x80000 | 0x2000, 0x30);
        for (i = 0; i < 600; i++) {
            RdpVtx v[3];
            int k;
            float cx, cy, sz;
            seed = seed * 1103515245u + 12345u; cx = (float)(seed >> 16 & 511) * 320 / 512;
            seed = seed * 1103515245u + 12345u; cy = (float)(seed >> 16 & 511) * 240 / 512;
            sz = 30;
            for (k = 0; k < 3; k++) {
                memset(&v[k], 0, sizeof v[k]);
                v[k].x = cx + (k == 1 ? sz : 0); v[k].y = cy + (k == 2 ? sz : 0);
                v[k].z = (float)(i % 97) / 100; v[k].iw = 1.0f / (1 + k * 0.1f);
                v[k].r = v[k].g = v[k].b = v[k].a = 200;
                v[k].s = k == 1 ? 32 : 0; v[k].t = k == 2 ? 32 : 0;
            }
            rdp_triangle(&r, v, 1, 0, 1);
        }
    }
    t = now() - t0;
    printf("RDP %s : %d images de 600 triangles en %.3f s = %.1f ms/image, %.1f Mpixels/s\n",
           cycle2 ? "2 cycles" : "1 cycle", frames, t, t * 1000 / frames, r.pixels / t / 1e6);
    return 0;
}
