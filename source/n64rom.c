/* n64rom.c */
#include "n64rom.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *const n64_save_names[SAVE_COUNT] = {
    "AUCUNE", "EEPROM 4 KBIT", "EEPROM 16 KBIT", "SRAM 256 KBIT", "FLASHRAM 1 MBIT", "CONTROLLER PAK"
};
const char *const n64_order_names[4] = { ".Z64 (BIG-ENDIAN)", ".V64 (OCTETS PERMUTES)", ".N64 (LITTLE-ENDIAN)", "INCONNU" };

/* Quelques jeux connus (code unique a 2 lettres, octets 0x3C-0x3D). */
typedef struct { const char *id, *name; int save, players, expansion, rumble; } DbEntry;
static const DbEntry db[] = {
    { "SM", "Super Mario 64",                       SAVE_EEPROM4K,  1, 0, 0 },
    { "KT", "Mario Kart 64",                        SAVE_EEPROM4K,  4, 0, 0 },
    { "ZL", "The Legend of Zelda: Ocarina of Time", SAVE_SRAM,      1, 0, 1 },
    { "ZS", "The Legend of Zelda: Majora's Mask",   SAVE_FLASH,     1, 1, 1 },
    { "GE", "GoldenEye 007",                        SAVE_EEPROM4K,  4, 0, 1 },
    { "PD", "Perfect Dark",                         SAVE_EEPROM16K, 4, 2, 1 },
    { "AL", "Super Smash Bros.",                    SAVE_SRAM,      4, 0, 1 },
    { "FX", "Star Fox 64 / Lylat Wars",             SAVE_EEPROM4K,  4, 0, 1 },
    { "YS", "Yoshi's Story",                        SAVE_EEPROM16K, 1, 0, 1 },
    { "DY", "Diddy Kong Racing",                    SAVE_EEPROM16K, 4, 0, 1 },
    { "BK", "Banjo-Kazooie",                        SAVE_EEPROM4K,  1, 0, 0 },
    { "B7", "Banjo-Tooie",                          SAVE_EEPROM16K, 4, 0, 1 },
    { "FU", "Conker's Bad Fur Day",                 SAVE_EEPROM16K, 4, 0, 1 },
    { "DO", "Donkey Kong 64",                       SAVE_EEPROM16K, 4, 1, 1 },
    { "FZ", "F-Zero X",                             SAVE_SRAM,      4, 0, 1 },
    { "MQ", "Paper Mario",                          SAVE_FLASH,     1, 0, 0 },
    { "WR", "Wave Race 64",                         SAVE_EEPROM4K,  2, 0, 1 },
    { NULL, NULL, 0, 0, 0, 0 }
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint32_t n64_crc32(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

int n64_detect_order(const uint8_t *d, uint32_t n)
{
    if (n < 4) return ORDER_UNKNOWN;
    if (d[0] == 0x80 && d[1] == 0x37 && d[2] == 0x12 && d[3] == 0x40) return ORDER_Z64;
    if (d[0] == 0x37 && d[1] == 0x80 && d[2] == 0x40 && d[3] == 0x12) return ORDER_V64;
    if (d[0] == 0x40 && d[1] == 0x12 && d[2] == 0x37 && d[3] == 0x80) return ORDER_N64;
    return ORDER_UNKNOWN;
}

void n64_to_big_endian(uint8_t *d, uint32_t n, int order)
{
    uint32_t i;
    if (order == ORDER_V64) {
        for (i = 0; i + 1 < n; i += 2) { uint8_t t = d[i]; d[i] = d[i + 1]; d[i + 1] = t; }
    } else if (order == ORDER_N64) {
        for (i = 0; i + 3 < n; i += 4) {
            uint8_t a = d[i], b = d[i + 1];
            d[i] = d[i + 3]; d[i + 1] = d[i + 2]; d[i + 2] = b; d[i + 3] = a;
        }
    }
}

int n64_detect_cic(const uint8_t *d)
{
    uint32_t crc = n64_crc32(d + 0x40, 0x1000 - 0x40);
    if (n64_crc32(d + 0x40, 0xC00 - 0x40) == 0x587BD543u) return 5101;
    switch (crc) {
    case 0x6170A4A1u: return 6101;
    case 0x009E9EA3u: return 7102;
    case 0x90BB6CB5u: return 6102;
    case 0x0B050EE0u: return 6103;
    case 0x98BC2C86u: return 6105;
    case 0xACC8580Au: return 6106;
    }
    return 0;
}

static uint32_t rol(uint32_t v, unsigned s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }

int n64_checksum(const uint8_t *d, uint32_t n, int cic, uint32_t out[2])
{
    uint32_t seed, t1, t2, t3, t4, t5, t6, i;
    switch (cic) {
    case 6101: case 6102: case 7102: seed = 0xF8CA4DDCu; break;
    case 6103: seed = 0xA3886759u; break;
    case 6105: seed = 0xDF26F436u; break;
    case 6106: seed = 0x1FEA617Au; break;
    default: return -1;
    }
    if (n < N64_CHECK_BYTES) return -1;
    t1 = t2 = t3 = t4 = t5 = t6 = seed;
    for (i = 0x1000; i < N64_CHECK_BYTES; i += 4) {
        uint32_t dw = be32(d + i), r;
        if (t6 + dw < t6) t4++;
        t6 += dw;
        t3 ^= dw;
        r = rol(dw, dw & 0x1F);
        t5 += r;
        if (t2 > dw) t2 ^= r;
        else t2 ^= t6 ^ dw;
        if (cic == 6105) t1 += be32(d + 0x0750 + (i & 0xFF)) ^ dw;
        else t1 += t5 ^ dw;
    }
    if (cic == 6103) { out[0] = (t6 ^ t4) + t3; out[1] = (t5 ^ t2) + t1; }
    else if (cic == 6106) { out[0] = (t6 * t4) + t3; out[1] = (t5 * t2) + t1; }
    else { out[0] = t6 ^ t4 ^ t3; out[1] = t5 ^ t2 ^ t1; }
    return 0;
}

const char *n64_region_name(char r)
{
    switch (r) {
    case 'A': return "MONDE";       case 'B': return "BRESIL";     case 'C': return "CHINE";
    case 'D': return "ALLEMAGNE";   case 'E': return "AMERIQUE DU NORD";
    case 'F': return "FRANCE";      case 'G': return "GATEWAY (NTSC)"; case 'H': return "PAYS-BAS";
    case 'I': return "ITALIE";      case 'J': return "JAPON";      case 'K': return "COREE";
    case 'L': return "GATEWAY (PAL)"; case 'N': return "CANADA";
    case 'P': case 'X': case 'Y': case 'Z': return "EUROPE";
    case 'S': return "ESPAGNE";     case 'U': return "AUSTRALIE";  case 'W': return "SCANDINAVIE";
    }
    return "INCONNUE";
}

int n64_region_is_pal(char r)
{
    return strchr("DFHILPSUWXYZ", r) != NULL && r != 0;
}

int n64_parse(const uint8_t *d, uint32_t n, uint32_t total, int order, N64Info *o)
{
    int i;
    memset(o, 0, sizeof *o);
    if (n < 0x1000) return -1;
    o->order = order;
    o->size = total;
    for (i = 0; i < 20; i++) {
        uint8_t c = d[0x20 + i];
        o->internal[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    o->internal[20] = 0;
    for (i = 19; i >= 0 && o->internal[i] == ' '; i--) o->internal[i] = 0;
    for (i = 0; i < 4; i++) {
        uint8_t c = d[0x3B + i];
        o->code[i] = isalnum(c) ? (char)c : '?';
    }
    o->code[4] = 0;
    o->region_letter = (char)d[0x3E];
    o->version = d[0x3F];
    o->entry = be32(d + 0x08);
    if (d[0x0E]) snprintf(o->libultra, sizeof o->libultra, "%d.%d%c", d[0x0E] / 10, d[0x0E] % 10, isalpha(d[0x0F]) ? d[0x0F] : ' ');
    o->crc_hdr[0] = be32(d + 0x10);
    o->crc_hdr[1] = be32(d + 0x14);
    o->cic = n64_detect_cic(d);
    if (n64_checksum(d, n, o->cic, o->crc_calc) == 0)
        o->checksum = o->crc_calc[0] == o->crc_hdr[0] && o->crc_calc[1] == o->crc_hdr[1];
    else
        o->checksum = -1;

    for (i = 0; db[i].id; i++) {
        if (d[0x3C] == (uint8_t)db[i].id[0] && d[0x3D] == (uint8_t)db[i].id[1]) {
            o->known = 1;
            snprintf(o->name, sizeof o->name, "%s", db[i].name);
            o->save = db[i].save;
            o->players = db[i].players;
            o->expansion = db[i].expansion;
            o->rumble = db[i].rumble;
            break;
        }
    }
    return 0;
}
