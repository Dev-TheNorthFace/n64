/* host_main.c : banc d'essai PC de l'interface N64 (ROMs synthetiques : en-tete seul). */
#include "../source/common.h"
#include "../source/ui.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ROMDIR "/tmp/n64_roms"
static const char *dirs[] = { ROMDIR, NULL };
const char *const *plat_rom_dirs(void) { return dirs; }
const char *plat_data_dir(void) { return "/tmp/n64_data"; }
void plat_audio_push(const int16_t *s, int n) { (void)s; (void)n; }
void plat_perf(int *c, int *w) { *c = *w = 0; }
int plat_rsx_blit(void) { return 0; }
void *plat_gfx_worker(void) { return NULL; }
int plat_jit_attach(void *cpu, const char **why) { (void)cpu; *why = "PC : NON DISPONIBLE"; return -1; }
uint64_t plat_time_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }

int plat_list_dir(const char *dir, DirCallback cb, void *user)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[600];
    if (!d) return -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        cb(path, user);
    }
    closedir(d);
    return 0;
}

static void make_rom(const char *name, const char *title, const char *code4, int order, uint32_t size)
{
    uint8_t *d = malloc(size);
    uint32_t i, x = (uint32_t)size ^ (uint32_t)name[0];
    char p[300];
    FILE *f;
    for (i = 0; i < size; i++) { x = x * 1103515245u + 12345u; d[i] = (uint8_t)(x >> 16); }
    d[0] = 0x80; d[1] = 0x37; d[2] = 0x12; d[3] = 0x40;
    d[8] = 0x80; d[9] = 0x00; d[10] = 0x04; d[11] = 0x00;
    d[0x0E] = 0x14; d[0x0F] = 'K';
    memset(d + 0x20, ' ', 20);
    memcpy(d + 0x20, title, strlen(title));
    memcpy(d + 0x3B, code4, 4);
    d[0x3F] = 0;
    if (order == 1) for (i = 0; i < size; i += 2) { uint8_t t = d[i]; d[i] = d[i + 1]; d[i + 1] = t; }
    if (order == 2) for (i = 0; i < size; i += 4) { uint8_t a = d[i], b = d[i + 1]; d[i] = d[i + 3]; d[i + 1] = d[i + 2]; d[i + 2] = b; d[i + 3] = a; }
    snprintf(p, sizeof p, "%s/%s", ROMDIR, name);
    f = fopen(p, "wb");
    fwrite(d, 1, size, f);
    fclose(f);
    free(d);
}

static Surface surf;
static Pad pads[2];
static uint32_t prev;

static void shot(const char *name)
{
    char path[256];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "shots/%s.ppm", name);
    f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", surf.w, surf.h);
    for (i = 0; i < surf.w * surf.h; i++) {
        uint32_t c = surf.px[i];
        fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f);
    }
    fclose(f);
    printf("capture %-22s ecran %d\n", name, ui_screen());
}
static void frame(uint32_t h) { pads[0].held = h; pads[0].pressed = h & ~prev; prev = h; ui_frame(&surf, pads); }
static void idle(int n) { while (n--) frame(0); }
static void tap(uint32_t b) { frame(b); frame(0); }
static int kx, ky;
static void type_char(char c)
{
    static const char *rows[] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789:'-." };
    int r, col = -1;
    for (r = 0; r < 4 && col < 0; r++) { const char *p = strchr(rows[r], c); if (p) col = (int)(p - rows[r]); }
    r--;
    while (ky < r) { tap(B_DOWN); ky++; }
    while (ky > r) { tap(B_UP); ky--; }
    while (kx < col) { tap(B_RIGHT); kx++; }
    while (kx > col) { tap(B_LEFT); kx--; }
    tap(B_CROSS);
}
static void search(const char *q) { tap(B_CIRCLE); tap(B_TRIANGLE); kx = ky = 0; while (*q) type_char(*q++); tap(B_START); }

int main(void)
{
    int i;
    mkdir(ROMDIR, 0777);
    mkdir("/tmp/n64_data", 0777);
    mkdir("/tmp/n64_data/saves", 0777);
    mkdir("/tmp/n64_data/states", 0777);
    make_rom("Super Mario 64 (USA) [test].z64", "SUPER MARIO 64", "NSME", 0, 8 << 20);
    make_rom("Mario Kart 64 (Europe) [test].v64", "MARIOKART64", "NKTP", 1, 12 << 20);
    make_rom("Zelda OoT (Japan) [test].n64", "THE LEGEND OF ZELDA", "CZLJ", 2, 16 << 20);
    make_rom("homebrew_demo.z64", "MY HOMEBREW DEMO", "N??E", 0, 2 << 20);
    {   FILE *f = fopen(ROMDIR "/pas_une_rom.bin", "wb"); fputs("bonjour", f); fclose(f); }
    remove("/tmp/n64_data/config.ini");

    surf.w = 1280; surf.h = 720; surf.pitch = 1280;
    surf.px = calloc((size_t)surf.w * surf.h, 4);
    pads[0].connected = 1;
    ui_init();
    idle(50); shot("01_titre");
    tap(B_CROSS); idle(5); shot("02_bibliotheque");
    tap(B_DOWN); idle(5); shot("03_fiche_v64");
    search("ZELDA"); idle(3); shot("04_recherche");
    tap(B_CROSS); for (i = 0; i < 30; i++) frame(0); shot("05_demarrage");
    tap(B_CIRCLE); tap(B_CIRCLE); idle(2);
    tap(B_R1); tap(B_R1); tap(B_R1); tap(B_R1); idle(2); shot("06_filtre_europe");
    tap(B_CIRCLE); tap(B_START); idle(2); shot("07_options");
    for (i = 0; i < 4; i++) tap(B_DOWN);
    tap(B_CROSS); idle(2);
    pads[0].lx = 0.55f; pads[0].ly = -0.6f; pads[0].rx = 0.9f;
    frame(B_CROSS | B_L2 | B_R1 | B_UP); shot("08_manette");
    pads[0].lx = pads[0].ly = pads[0].rx = 0;
    frame(B_SELECT); frame(B_SELECT | B_CIRCLE); frame(0); idle(2);       /* retour aux options */
    tap(B_CIRCLE); idle(2);                                               /* bibliotheque */

    /* demo maison : demarrage, lancement, deplacement */
    tap(B_CIRCLE); search("DEMO"); idle(2);
    tap(B_CROSS); idle(5); shot("09_demo_verifications");
    tap(B_CROSS); for (i = 0; i < 40; i++) frame(0); shot("10_demo_execution");
    pads[0].lx = 0.9f; pads[0].ly = 0.9f;
    for (i = 0; i < 40; i++) frame(B_CROSS);
    pads[0].lx = pads[0].ly = 0;
    shot("11_demo_manette");
    tap(B_SELECT); idle(2); shot("12_menu");
    tap(B_DOWN); tap(B_DOWN); tap(B_CROSS); idle(2);              /* sauver l'etat 0 */
    for (i = 0; i < 10; i++) frame(0);
    tap(B_SELECT); idle(1);
    for (i = 0; i < 3; i++) tap(B_DOWN);
    tap(B_CROSS); idle(2); shot("12b_etat_charge");              /* charger l'etat 0 */
    tap(B_SELECT); idle(1);
    for (i = 0; i < 8; i++) tap(B_DOWN);
    tap(B_CROSS); idle(2);

    /* ROM synthetique (code aleatoire) : l'emulateur ne doit pas planter */
    tap(B_CIRCLE); search("MARIO"); idle(2); tap(B_DOWN); idle(1);
    tap(B_CROSS); for (i = 0; i < 20; i++) frame(0);
    tap(B_CROSS); for (i = 0; i < 30; i++) frame(0); shot("13_rom_sans_image");
    tap(B_SELECT); for (i = 0; i < 4; i++) tap(B_DOWN); tap(B_CROSS); idle(2);
    ui_shutdown();
    return 0;
}
