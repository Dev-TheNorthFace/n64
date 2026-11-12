/* romrun.c : fait tourner une vraie ROM sur PC (meme coeur que la PS3).
 *   romrun <rom> <trames> [script]
 * script : "trame:boutons,..." boutons = START A B Z L R UP DOWN LEFT RIGHT
 *          SX=val SY=val (stick), "-" = relacher tout, "SHOT" = capture.
 * Captures : shots/rom_<trame>.ppm ; son : shots/rom_son.raw (48 kHz stereo). */
#include "../source/n64.h"
#include "../source/n64rom.h"
#include "../source/jit.h"
int jit_pc_attach(VR4300 *c);
#include "ppc_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static N64 m;
static FILE *snd;
static uint64_t snd_frames;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t clk(void) { return (uint64_t)(now() * 1e6); }

static void audio(void *ctx, const int16_t *st, int n) { (void)ctx; if (snd) fwrite(st, 4, (size_t)n, snd); snd_frames += (uint64_t)n; }

static void shot(int f)
{
    char name[64];
    FILE *o;
    int i;
    if (!m.fb_valid) { printf("  [trame %d] pas d'image VI\n", f); return; }
    snprintf(name, sizeof name, "shots/rom_%05d.ppm", f);
    o = fopen(name, "wb");
    if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", m.fb_w, m.fb_h);
    for (i = 0; i < m.fb_w * m.fb_h; i++) {
        uint8_t c[3] = { (uint8_t)(m.fb[i] >> 16), (uint8_t)(m.fb[i] >> 8), (uint8_t)m.fb[i] };
        fwrite(c, 1, 3, o);
    }
    fclose(o);
}

static uint16_t parse_buttons(const char *s, int8_t *sx, int8_t *sy)
{
    uint16_t b = 0;
    char tok[32];
    while (*s) {
        int n = 0;
        while (*s && *s != '+' && n < 31) tok[n++] = *s++;
        tok[n] = 0;
        if (*s == '+') s++;
        if (!strcmp(tok, "A")) b |= 0x8000;
        else if (!strcmp(tok, "B")) b |= 0x4000;
        else if (!strcmp(tok, "Z")) b |= 0x2000;
        else if (!strcmp(tok, "START")) b |= 0x1000;
        else if (!strcmp(tok, "UP")) b |= 0x0800;
        else if (!strcmp(tok, "DOWN")) b |= 0x0400;
        else if (!strcmp(tok, "LEFT")) b |= 0x0200;
        else if (!strcmp(tok, "RIGHT")) b |= 0x0100;
        else if (!strcmp(tok, "L")) b |= 0x0020;
        else if (!strcmp(tok, "R")) b |= 0x0010;
        else if (!strncmp(tok, "SX=", 3)) *sx = (int8_t)atoi(tok + 3);
        else if (!strncmp(tok, "SY=", 3)) *sy = (int8_t)atoi(tok + 3);
    }
    return b;
}

static void status(int f, double t)
{
    printf("[trame %5d] pc %08X gfx %u (%s) tris %u/%u inconnues %u | son %u (%s, inconnues %u) | VI %u exc %llu | attente %llu%% | %.0f trames/s\n",
           f, (unsigned)m.cpu.pc, m.gfx_tasks, m.ucode >= 0 ? gsp_ucode_name(m.ucode) : "-",
           m.gsp.tris_in, m.gsp.tris_culled, m.gsp.unknown, m.audio_tasks,
           m.audio_abi >= 0 ? aspd_abi_name(m.audio_abi) : "-", m.aspd.unknown, m.vi_intrs,
           (unsigned long long)m.cpu.exceptions,
           (unsigned long long)(m.cpu.cycles ? m.cpu.idle_cycles * 100 / m.cpu.cycles : 0), f / t);
    if (m.gsp.unknown) printf("    derniere commande graphique inconnue : %08X\n", m.gsp.last_unknown);
}

int main(int argc, char **argv)
{
    FILE *f;
    long sz;
    uint8_t *rom;
    N64Info info;
    int frames, fr = 0, order;
    const char *script = argc > 3 ? argv[3] : "";
    double t0;
    if (argc < 3) { fprintf(stderr, "usage : romrun rom trames [script]\n"); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    rom = (uint8_t *)malloc((size_t)sz);
    if (fread(rom, 1, (size_t)sz, f) != (size_t)sz) return 1;
    fclose(f);
    order = n64_detect_order(rom, (uint32_t)sz);
    n64_to_big_endian(rom, (uint32_t)sz, order);
    n64_parse(rom, (uint32_t)sz, (uint32_t)sz, 0, &info);
    printf("ROM : %s (%s) CIC %d, somme %s, sauvegarde %s\n", info.known ? info.name : info.internal, info.code, info.cic,
           info.checksum == 1 ? "OK" : "DIFFERENTE", n64_save_names[info.save]);
    if (n64_init(&m, rom, (uint32_t)sz, info.cic, n64_region_is_pal(info.region_letter), info.expansion == 1, info.known ? info.save : SAVE_EEPROM4K)) return 1;
    if (getenv("JIT")) { if (jit_pc_attach(&m.cpu)) { printf("JIT impossible\n"); return 1; } printf("recompilateur actif (code PowerPC execute par l'interpreteur de test)\n"); }
    n64_boot_hle(&m);
    if (getenv("THREAD")) { N64Worker *pc_worker_create(void); m.gfx_worker = pc_worker_create(); printf("graphismes sur un 2e fil\n"); }
    if (getenv("CPI")) m.cpu.cpi = (unsigned)atoi(getenv("CPI"));
    if (getenv("NOIDLE")) m.cpu.idle_skip = 0;
    if (getenv("FAST")) m.rdp.fast = atoi(getenv("FAST"));
    if (getenv("SKIP")) m.frameskip = atoi(getenv("SKIP"));
    m.clock_us = clk;
    snd = fopen("shots/rom_son.raw", "wb");
    m.audio_out = audio;
    frames = atoi(argv[2]);
    t0 = now();
    while (fr < frames) {
        int guard = 0;
        /* script */
        const char *p = script;
        while (*p) {
            int at = atoi(p);
            const char *c = strchr(p, ':'), *e;
            char act[128];
            size_t n;
            if (!c) break;
            e = strchr(c, ',');
            n = e ? (size_t)(e - c - 1) : strlen(c + 1);
            if (n > 127) n = 127;
            memcpy(act, c + 1, n); act[n] = 0;
            if (at == fr) {
                if (!strcmp(act, "SHOT")) shot(fr);
                else if (!strcmp(act, "-")) { m.pad_buttons[0] = 0; m.pad_x[0] = m.pad_y[0] = 0; }
                else { int8_t sx = 0, sy = 0; m.pad_buttons[0] = parse_buttons(act, &sx, &sy); m.pad_x[0] = sx; m.pad_y[0] = sy; }
            }
            if (!e) break;
            p = e + 1;
        }
        while (!n64_run(&m, 16) && guard++ < 100000) {}
        fr++;
        if (getenv("PACE")) {                    /* cadence temps reel (comme la PS3) */
            static double next;
            double t = now();
            if (next == 0) next = t;
            next += atoi(getenv("PACE")) * 1e-6;
            if (t < next) { struct timespec ts = { 0, (long)((next - t) * 1e9) }; nanosleep(&ts, NULL); }
        }
        if (fr % 100 == 0) status(fr, now() - t0);
    }
    shot(fr);
    status(fr, now() - t0);
    {
        double tt = (now() - t0) * 1e6;
        printf("temps : CPU %.0f%%  3D %.0f%%  son %.0f%%  (total %.2f s)\n", (tt - m.t_gfx - m.t_audio) * 100 / tt,
               m.t_gfx * 100 / tt, m.t_audio * 100 / tt, tt / 1e6);
    }
    if (m.cpu.jit) printf("recompilateur : %llu blocs, %llu instructions MIPS par le code genere (%.0f %%)\n", (unsigned long long)m.cpu.jit_blocks,
                          (unsigned long long)m.cpu.jit_instr, m.cpu.instructions ? m.cpu.jit_instr * 100.0 / m.cpu.instructions : 0.0);
#ifdef JIT_STATS
    if (m.cpu.jit) {
        extern uint64_t jit_stat_op[64], jit_stat_cop1[64];
        uint64_t jit_stat_calls(VR4300 *c);
        int k;
        printf("blocs executes : %llu (%.1f instructions MIPS par bloc)\n", (unsigned long long)jit_stat_calls(&m.cpu), (double)m.cpu.jit_instr / (double)jit_stat_calls(&m.cpu));
        printf("interpretees (op : nombre) :");
        for (k = 0; k < 64; k++) if (jit_stat_op[k] > m.cpu.jit_instr / 1000) printf(" %02X:%llu", k, (unsigned long long)jit_stat_op[k]);
        printf("\n COP1 :");
        for (k = 0; k < 32; k++) if (jit_stat_cop1[k]) printf(" %02X:%llu", k, (unsigned long long)jit_stat_cop1[k]);
        printf("\n");
    }
#endif
    if (m.cpu.jit) { uint32_t u, sz; uint64_t fl, co; jit_info(&m.cpu, &u, &sz, &fl, &co); printf("zone de code : %u Ko utilises sur %u Ko, %llu vidages, %llu blocs traduits\n", u >> 10, sz >> 10, (unsigned long long)fl, (unsigned long long)co); }
    if (m.cpu.jit) printf("code PowerPC execute : %llu instructions, %.1f par instruction MIPS\n", (unsigned long long)ppc_executed,
                          m.cpu.jit_instr ? (double)ppc_executed / (double)m.cpu.jit_instr : 0.0);
    printf("son : %llu echantillons stereo produits (%.1f s)\n", (unsigned long long)snd_frames, snd_frames / 48000.0);
    if (snd) fclose(snd);
    n64_free(&m);
    free(rom);
    return 0;
}
