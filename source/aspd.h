/* aspd.h : microcode audio du RSP emule en haut niveau (HLE).
 * Execute les "listes audio" : decodage ADPCM, reechantillonnage,
 * enveloppes de volume, mixage, entrelacement stereo.
 *  - ABI 1 ("audio" standard) : Super Mario 64 et beaucoup de jeux 1996-97
 *  - NEAD (ABI 2) : Mario Kart 64, Zelda Ocarina of Time / Majora's Mask */
#ifndef N64_ASPD_H
#define N64_ASPD_H

#include <stdint.h>

enum { AUDIO_ABI_UNKNOWN = 0, AUDIO_ABI1 = 1, AUDIO_NEAD_MK = 2, AUDIO_NEAD_OOT = 3 };

typedef struct {
    uint8_t buf[0x1000];           /* memoire de travail (DMEM), octets big-endian */
    uint32_t seg[16];
    uint16_t in, out, count;
    uint16_t dry_right, wet_left, wet_right;
    int16_t dry, wet;
    int16_t vol[2], target[2];
    int32_t rate[2];
    uint32_t loop;
    int16_t table[16 * 16];        /* livre de codes ADPCM */
    uint16_t env_values[3], env_steps[3];
    uint32_t filter_count, filter_lut[2];
    /* statistiques */
    uint32_t lists, cmds, unknown;
} Aspd;

/* reconnait l'ABI a partir du segment de donnees du microcode et du code du jeu */
int  aspd_detect(const uint8_t *rdram, uint32_t size, uint32_t ucode_data, const char *game_id2);
void aspd_run(Aspd *a, uint8_t *rdram, uint32_t size, int abi, uint32_t alist, uint32_t alist_size);
const char *aspd_abi_name(int abi);

#endif
