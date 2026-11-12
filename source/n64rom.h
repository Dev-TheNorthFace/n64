/* n64rom.h : lecture et analyse des ROMs Nintendo 64.
 *  - 3 ordres d'octets : .z64 (big-endian), .v64 (octets permutes par 2), .n64 (little-endian)
 *  - en-tete : titre interne, code du jeu, region, version, adresse de demarrage
 *  - CIC : identifie par le CRC32 de l'IPL3 (0x40-0xFFF)
 *  - somme de controle IPL3 sur 1 Mo a partir de 0x1000 (graine propre a chaque CIC) */
#ifndef N64_ROM_H
#define N64_ROM_H

#include <stdint.h>

#define N64_CHECK_BYTES 0x101000u     /* en-tete + IPL3 + 1 Mo controle */

enum { ORDER_Z64 = 0, ORDER_V64, ORDER_N64, ORDER_UNKNOWN };
enum { SAVE_NONE = 0, SAVE_EEPROM4K, SAVE_EEPROM16K, SAVE_SRAM, SAVE_FLASH, SAVE_CPAK, SAVE_COUNT };

typedef struct {
    int order;
    uint32_t size;
    char internal[24];      /* titre interne (0x20, 20 caracteres) */
    char code[5];           /* ex. "NSME" : categorie + code unique + region */
    char region_letter;
    int version;
    uint32_t entry;         /* adresse de demarrage (0x08) */
    char libultra[8];
    int cic;                /* 6101, 6102, 6103, 6105, 6106, 7102, 5101 ou 0 */
    uint32_t crc_hdr[2];    /* somme stockee (0x10/0x14) */
    uint32_t crc_calc[2];   /* somme recalculee */
    int checksum;           /* 1 = OK, 0 = differente, -1 = non verifiable */
    /* base de donnees */
    int known;
    char name[48];
    int save;
    int players;
    int expansion;          /* 1 = Expansion Pak necessaire, 2 = conseille */
    int rumble;
} N64Info;

extern const char *const n64_save_names[SAVE_COUNT];
extern const char *const n64_order_names[4];

int  n64_detect_order(const uint8_t *d, uint32_t n);
void n64_to_big_endian(uint8_t *d, uint32_t n, int order);
/* analyse ; 'd' doit etre deja en big-endian, 'n' >= 0x1000 (0x101000 pour la somme) */
int  n64_parse(const uint8_t *d, uint32_t n, uint32_t total_size, int order, N64Info *out);
int  n64_detect_cic(const uint8_t *d);
int  n64_checksum(const uint8_t *d, uint32_t n, int cic, uint32_t out[2]);
const char *n64_region_name(char r);
int  n64_region_is_pal(char r);
uint32_t n64_crc32(const uint8_t *d, uint32_t n);

#endif
