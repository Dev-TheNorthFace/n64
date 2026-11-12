/* bench.c : mesure de vitesse sur PC (demo maison) : CPU seul, puis CPU + graphismes. */
#include "../source/n64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const uint8_t demo_rom[];
extern const uint32_t demo_rom_size;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void run(const char *name, int gfx, int frames)
{
    static N64 m;
    uint8_t *rom = malloc(demo_rom_size);
    double t0, t;
    int f = 0;
    memcpy(rom, demo_rom, demo_rom_size);
    n64_init(&m, rom, demo_rom_size, 6102, 0, 0, 0);
    n64_boot_hle(&m);
    m.gfx_enabled = gfx;
    m.pad_buttons[0] = 0x4000;
    t0 = now();
    while (f < frames) if (n64_run(&m, 16)) f++;
    t = now() - t0;
    printf("%-28s %4d trames en %.3f s = %6.0f trames/s (%5.0f %% temps reel)  %.1f M instr/s  %llu instr/trame\n",
           name, frames, t, frames / t, frames / t / 60 * 100, m.cpu.instructions / t / 1e6,
           (unsigned long long)(m.cpu.instructions / frames));
    n64_free(&m);
    free(rom);
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    run("CPU seul", 0, frames);
    run("CPU + graphismes + son", 1, frames);
    return 0;
}
