/* roms.h : bibliotheque de ROMs N64 et recherche. */
#ifndef N64_ROMS_H
#define N64_ROMS_H

#include <stdint.h>
#include "n64rom.h"

#define ROM_MAX 512

typedef struct {
    char path[256];
    char file[112];
    char title[64];         /* nom affiche */
    N64Info info;
    int valid;              /* en-tete N64 reconnu */
    int builtin;            /* demo maison integree */
} RomEntry;

typedef struct {
    RomEntry e[ROM_MAX];
    int count;
    unsigned char fav[ROM_MAX];
    int recent_rank[ROM_MAX];
    int view[ROM_MAX];
    int view_count;
} RomLib;

enum { RSORT_NAME = 0, RSORT_FAV, RSORT_RECENT, RSORT_COUNT };
enum { RCAT_ALL = 0, RCAT_FAV, RCAT_RECENT, RCAT_USA, RCAT_EUROPE, RCAT_JAPAN, RCAT_COUNT };
extern const char *const rom_categories[];

void rom_scan(RomLib *L);
void rom_filter(RomLib *L, const char *query, int category, int sort);
int  rom_find_file(const RomLib *L, const char *file);
/* lit au plus 'max' octets ; *total recoit la taille du fichier */
uint8_t *file_load_part(const char *path, uint32_t max, uint32_t *got, uint32_t *total);
uint8_t *file_load(const char *path, uint32_t *size, uint32_t max);
extern const uint8_t demo_rom[];
extern const uint32_t demo_rom_size;
void text_fold(const char *in, char *out, int outsz);

#endif
