/* audio_tests.c : tests du microcode audio HLE (PC). */
#include "../source/aspd.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM (1u << 20)
static uint8_t *ram;
static Aspd a;
static int fails, passes;
#define CHECK(cond, m) do { if (cond) passes++; else { fails++; printf("ECHEC: %s (ligne %d)\n", m, __LINE__); } } while (0)

#define AL 0x10000u
static uint32_t alp;
static void w16(uint32_t x, int v) { ram[x] = (uint8_t)((uint16_t)v >> 8); ram[x + 1] = (uint8_t)v; }
static void w32(uint32_t x, uint32_t v) { w16(x, (int)(v >> 16)); w16(x + 2, (int)(v & 0xFFFF)); }
static int16_t r16(uint32_t x) { return (int16_t)(ram[x] << 8 | ram[x + 1]); }
static void cmd(uint32_t w1, uint32_t w2) { w32(alp, w1); w32(alp + 4, w2); alp += 8; }
static void begin(void) { alp = AL; }
static void run(int abi) { aspd_run(&a, ram, RAM, abi, AL, alp - AL); }
static int16_t dm(uint32_t o) { return (int16_t)(a.buf[o] << 8 | a.buf[o + 1]); }

static void test_detect(void)
{
    memset(ram, 0, 0x100);
    w32(0x80, 1); w32(0x80 + 0x30, 0xF0000F00u);
    CHECK(aspd_detect(ram, RAM, 0x80000080u, "SM") == AUDIO_ABI1, "detection ABI1 (Mario 64)");
    w32(0x80 + 0x30, 0x12345678u);
    CHECK(aspd_detect(ram, RAM, 0x80000080u, "KT") == AUDIO_NEAD_MK, "detection NEAD Mario Kart");
    CHECK(aspd_detect(ram, RAM, 0x80000080u, "ZL") == AUDIO_NEAD_OOT, "detection NEAD Zelda");
    w32(0x80, 0x12);
    CHECK(aspd_detect(ram, RAM, 0x80000080u, "BK") == AUDIO_ABI_UNKNOWN, "ABI3 non gere");
}

static void test_load_save_mix_interleave(void)
{
    int i, ok = 1;
    memset(ram, 0, RAM);
    for (i = 0; i < 64; i++) { w16(0x20000 + (uint32_t)i * 2, i * 100); w16(0x20100 + (uint32_t)i * 2, -i * 50); }
    begin();
    cmd(0x08000000u | 0x000, 0x0100u << 16 | 128);       /* in 0, out 0x100, 128 octets */
    cmd(0x04000000u, 0x20000);                            /* LOADBUFF -> 0x5C0 */
    cmd(0x08000080u, 0x0100u << 16 | 128);
    cmd(0x04000000u, 0x20100);                            /* LOADBUFF -> 0x640 */
    cmd(0x02000200u, 128);                                /* efface 0x7C0 */
    cmd(0x08000000u, 0x0200u << 16 | 128);
    cmd(0x0C007FFFu, 0x0000u << 16 | 0x0200);             /* MIXER gain ~1 : 0x5C0 -> 0x7C0 */
    cmd(0x08000000u, 0x0300u << 16 | 128);
    cmd(0x0D000000u, 0x0000u << 16 | 0x0080);             /* INTERLEAVE 0x5C0/0x640 -> 0x8C0 */
    cmd(0x08000000u, 0x0300u << 16 | 256);
    cmd(0x06000000u, 0x30000);                            /* SAVEBUFF */
    run(AUDIO_ABI1);
    for (i = 0; i < 64; i++) {
        if (abs(dm(0x7C0 + (uint32_t)i * 2) - i * 100) > 1) ok = 0;
        if (r16(0x30000 + (uint32_t)i * 4) != i * 100 || r16(0x30002 + (uint32_t)i * 4) != -i * 50) ok = 0;
    }
    CHECK(ok, "LOADBUFF / MIXER / INTERLEAVE / SAVEBUFF");
}

static void test_adpcm(void)
{
    int i, ok = 1;
    memset(ram, 0, RAM);
    /* livre : predicteur 0 = tout a zero (les echantillons sont les residus) */
    begin();
    cmd(0x0B000020u, 0x40000);                            /* LOADADPCM 32 octets (zero) */
    /* trame : echelle 4, predicteur 0 ; nibbles 1,2,...,7,-8,-1... */
    ram[0x41000] = 0x40;
    for (i = 0; i < 8; i++) ram[0x41001 + i] = (uint8_t)(((i * 2 + 1) & 15) << 4 | ((i * 2 + 2) & 15));
    cmd(0x08000000u, 0x0100u << 16 | 64);
    cmd(0x04000000u, 0x41000);                            /* 9 octets (arrondis a 64) */
    cmd(0x08000000u, 0x0100u << 16 | 32);
    cmd(0x01010000u, 0x42000);                            /* ADPCM init */
    run(AUDIO_ABI1);
    for (i = 0; i < 16; i++) {
        int nib = (i + 1) & 15, v = (nib >= 8 ? nib - 16 : nib) * 16;
        if (dm(0x5C0 + 0x100 + 32 + (uint32_t)i * 2) != v) ok = 0;
    }
    CHECK(ok, "ADPCM : residus a l'echelle");
    for (i = 0; i < 16; i++) if (dm(0x5C0 + 0x100 + (uint32_t)i * 2) != 0) ok = 0;
    CHECK(ok, "ADPCM : historique initial nul en tete");
    CHECK(r16(0x42000 + 30) == 0 && r16(0x42000 + 28) == -16, "ADPCM : etat sauvegarde (16 derniers)");
    /* predicteur : book1[0] = 2048 (x1) => sortie = residu + echantillon precedent */
    w16(0x40000 + 16, 2048);
    for (i = 0; i < 8; i++) ram[0x41001 + i] = 0x11;      /* tous les residus = 1 << 0 */
    ram[0x41000] = 0x00;
    run(AUDIO_ABI1);
    CHECK(dm(0x5C0 + 0x100 + 32) == 1 && dm(0x5C0 + 0x100 + 34) == 2 && dm(0x5C0 + 0x100 + 48) == 3 && dm(0x5C0 + 0x100 + 62) == 2, "ADPCM : prediction ordre 2 (sans etat)");
    /* reprise depuis l'etat : historique non nul */
    begin();
    cmd(0x0B000020u, 0x40000);
    cmd(0x08000000u, 0x0100u << 16 | 64);
    cmd(0x04000000u, 0x41000);
    cmd(0x08000000u, 0x0100u << 16 | 32);
    for (i = 0; i < 16; i++) w16(0x43000 + (uint32_t)i * 2, 100);
    cmd(0x01000000u, 0x43000);                            /* ADPCM sans init */
    run(AUDIO_ABI1);
    CHECK(dm(0x5C0 + 0x100 + 32) == 101, "ADPCM : reprise depuis l'etat sauvegarde");
}

static void test_resample(void)
{
    int i, ok = 1, peak = 0;
    memset(ram, 0, RAM);
    for (i = 0; i < 512; i++) w16(0x20000 + (uint32_t)i * 2, (int)(10000 * sin(i * 2 * M_PI / 64)));
    begin();
    cmd(0x08000020u, 0x0000u << 16 | 1024);
    cmd(0x04000000u, 0x20000);
    cmd(0x08000020u, 0x0500u << 16 | 256);
    cmd(0x05018000u, 0x50000);                            /* pas 1.0, init */
    cmd(0x08000020u, 0x0600u << 16 | 256);
    cmd(0x05014000u, 0x50100);                            /* pas 0.5 : 2x plus d'echantillons */
    run(AUDIO_ABI1);
    for (i = 4; i < 128; i++) {
        int want = (int)(10000 * sin((i - 3) * 2 * M_PI / 64));
        if (abs(dm(0x5C0 + 0x500 + (uint32_t)i * 2) - want) > 600) ok = 0;
    }
    CHECK(ok, "RESAMPLE pas 1 : copie retardee de 3 echantillons");
    for (i = 0; i < 128; i++) if (abs(dm(0x5C0 + 0x600 + (uint32_t)i * 2)) > peak) peak = abs(dm(0x5C0 + 0x600 + (uint32_t)i * 2));
    CHECK(peak > 9000, "RESAMPLE pas 0.5 : signal conserve");
    CHECK((uint16_t)r16(0x50108) == 0 && r16(0x50100) != 0, "RESAMPLE : etat sauvegarde");
}

static void test_envmixer(void)
{
    int i, ok = 1;
    memset(ram, 0, RAM);
    for (i = 0; i < 64; i++) w16(0x20000 + (uint32_t)i * 2, 8000);
    begin();
    cmd(0x08000000u, 0x0000u << 16 | 128);
    cmd(0x04000000u, 0x20000);
    cmd(0x02000100u, 128); cmd(0x02000200u, 128);
    cmd(0x09064000u, 0); cmd(0x09042000u, 0);              /* vol G 0.5, D 0.25 */
    cmd(0x09024000u, 0x10000); cmd(0x09002000u, 0x10000);  /* cibles identiques */
    cmd(0x09087FFFu, 0);                                    /* sec ~1, effet 0 */
    cmd(0x08080200u, 0x0300u << 16 | 0x0380);
    cmd(0x08000000u, 0x0100u << 16 | 128);
    cmd(0x03010000u, 0x60000);
    run(AUDIO_ABI1);
    for (i = 0; i < 64; i++) {
        if (abs(dm(0x5C0 + 0x100 + (uint32_t)i * 2) - 4000) > 20) ok = 0;
        if (abs(dm(0x5C0 + 0x200 + (uint32_t)i * 2) - 2000) > 20) ok = 0;
    }
    CHECK(ok, "ENVMIXER : volumes gauche/droite");
}

static void test_nead(void)
{
    int i, ok = 1;
    memset(ram, 0, RAM);
    for (i = 0; i < 64; i++) w16(0x20000 + (uint32_t)i * 2, 16000);
    begin();
    cmd(0x14000000u | (128u << 12) | 0x100, 0x20000);       /* LOADBUFF 128 octets -> 0x100 */
    cmd(0x02000400u, 0x200);                                 /* efface 0x400..0x5FF */
    cmd(0x12000000u, 0);                                     /* ENVSETUP1 : wet 0, pas 0 */
    cmd(0x16000000u, 0x80004000u);                           /* ENVSETUP2 : G 0.5, D 0.25 */
    /* ENVMIXER : in 0x100, 64 echantillons, dl 0x400 dr 0x480 wl 0x500 wr 0x580 */
    cmd(0x13000000u | (0x10u << 16) | (64u << 8), (0x40u << 24) | (0x48u << 16) | (0x50u << 8) | 0x58u);
    cmd(0x0D000000u | (128u << 12) | 0x600, 0x0400u << 16 | 0x0480);   /* INTERLEAVE (Zelda) */
    run(AUDIO_NEAD_OOT);
    for (i = 0; i < 64; i++) {
        if (abs(dm(0x400 + (uint32_t)i * 2) - 8000) > 2 || abs(dm(0x480 + (uint32_t)i * 2) - 4000) > 2) ok = 0;
        if (dm(0x500 + (uint32_t)i * 2) != 0) ok = 0;
    }
    CHECK(ok, "NEAD ENVMIXER");
    CHECK(dm(0x600) == 8000 && dm(0x602) == 4000, "NEAD INTERLEAVE");
    CHECK(a.unknown == 0, "NEAD : aucune commande inconnue");
}

static void test_fuzz(void)
{
    int i, abi;
    uint32_t seed = 99;
    for (abi = 1; abi <= 3; abi++) {
        for (i = 0; i < 0x2000; i++) { seed = seed * 1103515245u + 12345u; ram[AL + (uint32_t)i] = (uint8_t)(seed >> 13); }
        aspd_run(&a, ram, RAM, abi, AL, 0x2000);
    }
    CHECK(1, "listes aleatoires sans plantage");
}

int main(void)
{
    ram = (uint8_t *)calloc(RAM, 1);
    memset(&a, 0, sizeof a);
    test_detect();
    test_load_save_mix_interleave();
    test_adpcm();
    test_resample();
    test_envmixer();
    test_nead();
    test_fuzz();
    free(ram);
    printf("Son : %d reussis, %d echecs\n", passes, fails);
    return fails ? 1 : 0;
}
