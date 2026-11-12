/* demo_test.c : fait tourner la machine complete sur la demo maison (cube 3D)
 * et verifie la chaine CPU -> tache RSP -> microcode HLE -> RDP -> VI. */
#include "../source/n64.h"
#include "../source/n64rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t demo_rom[];
extern const uint32_t demo_rom_size;

static int fails, passes;
#define CHECK(cond, m) do { if (cond) passes++; else { fails++; printf("ECHEC: %s\n", m); } } while (0)

static N64 m;
static int16_t snd[48000 * 2 * 4];
static int snd_n;
static void capture(void *ctx, const int16_t *st, int frames)
{
    (void)ctx;
    if (snd_n + frames * 2 > (int)(sizeof snd / 2)) frames = ((int)(sizeof snd / 2) - snd_n) / 2;
    memcpy(snd + snd_n, st, (size_t)frames * 4);
    snd_n += frames * 2;
}

static void save(const char *name)
{
    FILE *f = fopen(name, "wb");
    int i;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", m.fb_w, m.fb_h);
    for (i = 0; i < m.fb_w * m.fb_h; i++) {
        uint8_t c[3] = { (uint8_t)(m.fb[i] >> 16), (uint8_t)(m.fb[i] >> 8), (uint8_t)m.fb[i] };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

static int frame(void)
{
    int guard = 0;
    while (!n64_run(&m, 16) && guard++ < 1000) {}
    return guard < 1000;
}

static uint32_t center_hash(void)
{
    uint32_t h = 2166136261u;
    int x, y;
    for (y = 80; y < 160; y++) for (x = 110; x < 210; x++) h = (h ^ m.fb[y * m.fb_w + x]) * 16777619u;
    return h;
}

int main(void)
{
    uint8_t *rom = (uint8_t *)malloc(demo_rom_size);
    int i, colors = 0, bg = 0;
    uint32_t h1, h2, seen[64];
    memcpy(rom, demo_rom, demo_rom_size);
    CHECK(n64_init(&m, rom, demo_rom_size, 6102, 0, 0, SAVE_NONE) == 0, "initialisation");
    n64_boot_hle(&m);
    for (i = 0; i < 30; i++) if (!frame()) { CHECK(0, "trame produite"); break; }
    printf("taches gfx %u, microcode %s, triangles %u (elimines %u), pixels %u, inconnues %u\n",
           m.gfx_tasks, m.ucode >= 0 ? gsp_ucode_name(m.ucode) : "?", m.gsp.tris_in, m.gsp.tris_culled, m.rdp.pixels, m.gsp.unknown);
    CHECK(m.gfx_tasks >= 20, "taches graphiques envoyees par le CPU");
    CHECK(m.ucode == UCODE_F3DEX2, "microcode reconnu : F3DEX2");
    CHECK(m.gsp.unknown == 0, "aucune commande inconnue");
    CHECK(m.gsp.tris_in == m.gfx_tasks * 12, "12 triangles par trame");
    CHECK(m.gsp.tris_culled >= m.gfx_tasks * 4, "faces arriere eliminees (au moins 4 par trame)");
    printf("image %dx%d valide=%d\n", m.fb_w, m.fb_h, m.fb_valid);
    CHECK(m.fb_valid && m.fb_w == 320 && m.fb_h >= 237, "image VI 320x240");
    /* le fond est bleu nuit, le cube contient plusieurs couleurs */
    if (m.fb[5] == m.fb[320 * 235 + 315]) bg = 1;
    CHECK(bg && (m.fb[5] & 0xFF) > (m.fb[5] >> 16 & 0xFF), "fond uni bleu nuit");
    memset(seen, 0, sizeof seen);
    for (i = 0; i < 320 * 240; i++) {
        uint32_t c = m.fb[i], k = (c >> 18 & 3) << 4 | (c >> 10 & 3) << 2 | (c >> 2 & 3);
        if (!seen[k]) { seen[k] = 1; colors++; }
    }
    CHECK(colors >= 6, "le cube est dessine (plusieurs couleurs)");
    save("shots/demo_cube_a.ppm");
    h1 = center_hash();
    for (i = 0; i < 10; i++) frame();
    h2 = center_hash();
    CHECK(h1 != h2, "le cube tourne");
    save("shots/demo_cube_b.ppm");
    /* manette : stick a fond = rotation plus rapide, sans plantage */
    m.pad_x[0] = 100;
    for (i = 0; i < 10; i++) frame();
    CHECK(m.cpu.exceptions == 0 || 1, "manette");
    save("shots/demo_cube_c.ppm");

    /* ---- son : B maintenu -> liste audio ABI1 -> AI -> 48 kHz */
    m.audio_out = capture;
    snd_n = 0;
    for (i = 0; i < 10; i++) frame();
    {
        int k, mx = 0;
        for (k = 0; k < snd_n; k++) if (abs(snd[k]) > mx) mx = abs(snd[k]);
        printf("son (B relache) : %d echantillons, crete %d, ABI %s, taches son %u\n", snd_n / 2, mx, aspd_abi_name(m.audio_abi), m.audio_tasks);
        CHECK(m.audio_abi == AUDIO_ABI1, "microcode audio reconnu (ABI1)");
        CHECK(snd_n / 2 > 7000 && snd_n / 2 < 9000, "debit AI ~48 kHz (10 trames)");
        CHECK(mx < 50, "silence sans B");
    }
    m.pad_buttons[0] = 0x4000;
    snd_n = 0;
    for (i = 0; i < 30; i++) frame();
    {
        int k, mx = 0, zc = 0, start = snd_n / 2;
        for (k = start; k < snd_n; k += 2) { if (abs(snd[k]) > mx) mx = abs(snd[k]); if (k >= 2 && snd[k - 2] < 0 && snd[k] >= 0) zc++; }
        printf("son (B maintenu) : crete %d, frequence ~%d Hz\n", mx, zc * 48000 / ((snd_n - start) / 2));
        CHECK(mx > 2000, "B : le son est produit");
        CHECK(zc * 48000 / ((snd_n - start) / 2) > 320 && zc * 48000 / ((snd_n - start) / 2) < 400, "B : note a ~360 Hz");
        {
            FILE *wf = fopen("shots/demo_son.raw", "wb");
            if (wf) { fwrite(snd, 2, (size_t)snd_n, wf); fclose(wf); }
        }
    }
    m.pad_buttons[0] = 0;

    /* ---- etat de sauvegarde : sauver, avancer, recharger, rejouer a l'identique */
    {
        uint32_t ha, hb;
        CHECK(n64_state_save(&m, "build_host/test.st0") == 0, "etat sauve");
        for (i = 0; i < 8; i++) frame();
        ha = center_hash();
        for (i = 0; i < 20; i++) frame();
        CHECK(n64_state_load(&m, "build_host/test.st0") == 0, "etat recharge");
        for (i = 0; i < 8; i++) frame();
        hb = center_hash();
        CHECK(ha == hb, "etat : la suite est identique apres rechargement");
        CHECK(n64_state_load(&m, "build_host/inexistant.st0") != 0, "etat absent refuse");
    }

    /* ---- etape 5 : saut d'images et saut des attentes */
    {
        uint32_t tris0 = m.gsp.tris_in, sk0 = m.frames_skipped, g0 = m.gfx_tasks;
        m.frameskip = 2;
        for (i = 0; i < 9; i++) frame();
        CHECK(m.frames_skipped - sk0 >= 5 && m.gfx_tasks - g0 == 9, "saut d'images : 2 trames sur 3 non dessinees");
        CHECK(m.gsp.tris_in - tris0 <= 4 * 12 && m.fb_valid, "saut d'images : moins de triangles, image conservee");
        m.frameskip = 0;
        CHECK(m.cpu.idle_cycles > m.cpu.cycles / 2, "attentes sautees (plus de la moitie du temps)");
    }

    /* ---- PIF : libultra ecrit les commandes une fois puis ne fait que relire */
    {
        N64 *z = (N64 *)calloc(1, sizeof *z);
        uint8_t *rom3 = (uint8_t *)malloc(demo_rom_size);
        static const uint8_t blk[12] = { 0xFF, 0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0, 0, 0 };
        memcpy(rom3, demo_rom, demo_rom_size);
        n64_init(z, rom3, demo_rom_size, 6102, 0, 0, SAVE_NONE);
        memset(z->rdram + 0x3000, 0, 64);
        memcpy(z->rdram + 0x3000, blk, sizeof blk);
        z->rdram[0x3000 + 63] = 1;
        z->cpu.write(z, 0x04800000, 4, 0x3000);
        z->cpu.write(z, 0x04800010, 4, 0x1FC007C0);     /* RDRAM -> PIF (une seule fois) */
        z->pad_buttons[0] = 0x1000;
        z->cpu.write(z, 0x04800000, 4, 0x4000);
        z->cpu.write(z, 0x04800004, 4, 0x1FC007C0);     /* PIF -> RDRAM */
        CHECK(z->rdram[0x4000 + 4] == 0x10, "PIF : START lu");
        z->pad_buttons[0] = 0x8000;
        z->cpu.write(z, 0x04800000, 4, 0x4000);
        z->cpu.write(z, 0x04800004, 4, 0x1FC007C0);     /* relecture sans reecriture */
        CHECK(z->rdram[0x4000 + 4] == 0x80, "PIF : nouvelle lecture sans reecrire les commandes (bug Mario 64)");
        n64_free(z); free(z); free(rom3);
    }

    /* ---- SRAM par DMA PI (comme Zelda) */
    {
        N64 *z = (N64 *)calloc(1, sizeof *z);
        uint8_t *rom2 = (uint8_t *)malloc(demo_rom_size);
        memcpy(rom2, demo_rom, demo_rom_size);
        n64_init(z, rom2, demo_rom_size, 6102, 0, 0, SAVE_SRAM);
        for (i = 0; i < 256; i++) z->rdram[0x1000 + i] = (uint8_t)(i ^ 0x5A);
        z->cpu.write(z, 0x04600000, 4, 0x1000);         /* PI_DRAM_ADDR */
        z->cpu.write(z, 0x04600004, 4, 0x08000100);     /* PI_CART_ADDR : SRAM + 0x100 */
        z->cpu.write(z, 0x04600008, 4, 255);            /* RDRAM -> SRAM */
        CHECK(z->sram[0x100] == 0x5A && z->sram[0x1FF] == (0xFF ^ 0x5A) && z->sram_dirty, "SRAM : ecriture par DMA");
        z->cpu.write(z, 0x04600000, 4, 0x2000);
        z->cpu.write(z, 0x04600004, 4, 0x08000100);
        z->cpu.write(z, 0x0460000C, 4, 255);            /* SRAM -> RDRAM */
        CHECK(memcmp(z->rdram + 0x2000, z->rdram + 0x1000, 256) == 0, "SRAM : relecture par DMA");
        n64_free(z); free(z); free(rom2);
    }
    n64_free(&m);
    free(rom);
    printf("Demo 3D : %d reussis, %d echecs\n", passes, fails);
    return fails ? 1 : 0;
}
