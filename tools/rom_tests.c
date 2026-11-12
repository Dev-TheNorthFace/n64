/* rom_tests.c : tests de lecture des ROMs N64 (ROM synthetique, aucun jeu reel). */
#include "../source/n64rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, passes;
#define CHECK(c, m) do { if (c) passes++; else { fails++; printf("ECHEC: %s (ligne %d)\n", m, __LINE__); } } while (0)

static uint8_t *make_rom(uint32_t n)
{
    uint8_t *d = malloc(n);
    uint32_t i, x = 12345;
    for (i = 0; i < n; i++) { x = x * 1103515245u + 12345u; d[i] = (uint8_t)(x >> 16); }
    d[0] = 0x80; d[1] = 0x37; d[2] = 0x12; d[3] = 0x40;
    d[8] = 0x80; d[9] = 0x00; d[10] = 0x04; d[11] = 0x00;
    d[0x0E] = 0x14; d[0x0F] = 'L';
    memcpy(d + 0x20, "TEST ROM N64        ", 20);
    d[0x3B] = 'N'; d[0x3C] = 'S'; d[0x3D] = 'M'; d[0x3E] = 'P'; d[0x3F] = 1;
    return d;
}

int main(int argc, char **argv)
{
    const uint32_t N = 0x200000;
    uint8_t *z = make_rom(N), *v = malloc(N), *n = malloc(N);
    N64Info info;
    uint32_t i, sum[2];
    static const int cics[4] = { 6102, 6103, 6105, 6106 };

    /* ordres d'octets */
    for (i = 0; i < N; i += 4) {
        v[i] = z[i + 1]; v[i + 1] = z[i]; v[i + 2] = z[i + 3]; v[i + 3] = z[i + 2];
        n[i] = z[i + 3]; n[i + 1] = z[i + 2]; n[i + 2] = z[i + 1]; n[i + 3] = z[i];
    }
    CHECK(n64_detect_order(z, N) == ORDER_Z64, "detection .z64");
    CHECK(n64_detect_order(v, N) == ORDER_V64, "detection .v64");
    CHECK(n64_detect_order(n, N) == ORDER_N64, "detection .n64");
    n64_to_big_endian(v, N, ORDER_V64);
    n64_to_big_endian(n, N, ORDER_N64);
    CHECK(!memcmp(v, z, N), "conversion .v64 -> big-endian");
    CHECK(!memcmp(n, z, N), "conversion .n64 -> big-endian");

    /* en-tete */
    CHECK(n64_parse(z, N, N, ORDER_Z64, &info) == 0, "analyse");
    CHECK(!strcmp(info.internal, "TEST ROM N64"), "titre interne");
    CHECK(!strcmp(info.code, "NSMP"), "code du jeu");
    CHECK(info.region_letter == 'P' && n64_region_is_pal('P') && info.version == 1, "region / version");
    CHECK(info.entry == 0x80000400u, "adresse de demarrage");
    CHECK(info.known && info.save == SAVE_EEPROM4K, "base de donnees (SM -> EEPROM 4K)");
    CHECK(info.cic == 0 && info.checksum == -1, "IPL3 inconnu -> somme non verifiable");
    CHECK(!strcmp(info.libultra, "2.0L"), "version libultra");

    /* somme de controle : affiche les valeurs pour comparaison avec la reference Python */
    for (i = 0; i < 4; i++) {
        CHECK(n64_checksum(z, N, cics[i], sum) == 0, "calcul somme");
        printf("CIC %d %08X %08X\n", cics[i], sum[0], sum[1]);
    }
    if (argc > 1) { FILE *f = fopen(argv[1], "wb"); fwrite(z, 1, N, f); fclose(f); }
    free(z); free(v); free(n);
    printf("%d tests reussis, %d echecs\n", passes, fails);
    return fails != 0;
}
