/* roms.c : detection des ROMs N64, analyse de l'en-tete et recherche. */
#include "roms.h"
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const rom_categories[] = { "TOUS", "FAVORIS", "RECENTS", "USA", "EUROPE", "JAPON", NULL };

void text_fold(const char *in, char *out, int outsz)
{
    int o = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && o < outsz - 1) {
        unsigned char c = *s++;
        if (c == 0xC3 && *s) {
            unsigned char d = *s++;
            char r = '?';
            if ((d >= 0x80 && d <= 0x85) || (d >= 0xA0 && d <= 0xA5)) r = 'A';
            else if (d == 0x87 || d == 0xA7) r = 'C';
            else if ((d >= 0x88 && d <= 0x8B) || (d >= 0xA8 && d <= 0xAB)) r = 'E';
            else if ((d >= 0x8C && d <= 0x8F) || (d >= 0xAC && d <= 0xAF)) r = 'I';
            else if ((d >= 0x92 && d <= 0x96) || (d >= 0xB2 && d <= 0xB6)) r = 'O';
            else if ((d >= 0x99 && d <= 0x9C) || (d >= 0xB9 && d <= 0xBC)) r = 'U';
            out[o++] = r;
        } else if (c >= 0x80) {
            while ((*s & 0xC0) == 0x80) s++;
            out[o++] = '?';
        } else out[o++] = (char)toupper(c);
    }
    out[o] = 0;
}

uint8_t *file_load_part(const char *path, uint32_t max, uint32_t *got, uint32_t *total)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    size_t want;
    *got = 0;
    if (total) *total = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    if (total) *total = (uint32_t)n;
    want = (size_t)n < max ? (size_t)n : max;
    buf = (uint8_t *)malloc(want);
    if (buf && fread(buf, 1, want, f) != want) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *got = (uint32_t)want;
    return buf;
}

uint8_t *file_load(const char *path, uint32_t *size, uint32_t max)
{
    uint32_t total;
    uint8_t *b = file_load_part(path, max, size, &total);
    if (b && total > max) { free(b); *size = 0; return NULL; }
    return b;
}

static int rom_ext(const char *f)
{
    const char *d = strrchr(f, '.');
    return d && (!strcasecmp(d, ".z64") || !strcasecmp(d, ".n64") || !strcasecmp(d, ".v64") ||
                 !strcasecmp(d, ".rom") || !strcasecmp(d, ".bin"));
}

static void copy_str(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memmove(dst, src, l);
    dst[l] = 0;
}

static void make_title(RomEntry *e)
{
    if (e->info.known) { copy_str(e->title, sizeof e->title, e->info.name); return; }
    if (e->valid && e->info.internal[0]) {
        /* titre interne en majuscules -> casse de titre */
        int i, up = 1;
        copy_str(e->title, sizeof e->title, e->info.internal);
        for (i = 0; e->title[i]; i++) {
            char c = e->title[i];
            if (isalpha((unsigned char)c)) { e->title[i] = up ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c); up = 0; }
            else up = 1;
        }
        return;
    }
    {
        char *dot;
        size_t n = strlen(e->file);
        if (n >= sizeof e->title) n = sizeof e->title - 1;
        memcpy(e->title, e->file, n);
        e->title[n] = 0;
        dot = strrchr(e->title, '.');
        if (dot) *dot = 0;
    }
}

static void scan_cb(const char *path, void *user)
{
    RomLib *L = (RomLib *)user;
    const char *file = strrchr(path, '/');
    RomEntry *e;
    uint8_t *d;
    uint32_t got, total;
    int i, order;

    file = file ? file + 1 : path;
    if (!rom_ext(file) || L->count >= ROM_MAX) return;
    for (i = 0; i < L->count; i++) if (!strcasecmp(L->e[i].file, file)) return;
    d = file_load_part(path, N64_CHECK_BYTES, &got, &total);
    if (!d) return;
    order = n64_detect_order(d, got);
    if (order == ORDER_UNKNOWN || got < 0x1000) { free(d); return; }   /* pas une ROM N64 */
    n64_to_big_endian(d, got, order);

    e = &L->e[L->count++];
    memset(e, 0, sizeof *e);
    snprintf(e->path, sizeof e->path, "%s", path);
    snprintf(e->file, sizeof e->file, "%s", file);
    e->valid = n64_parse(d, got, total, order, &e->info) == 0;
    make_title(e);
    free(d);
}

void rom_scan(RomLib *L)
{
    const char *const *dirs = plat_rom_dirs();
    int i;
    memset(L, 0, sizeof *L);
    {
        RomEntry *e = &L->e[L->count++];
        strcpy(e->path, "(demo)");
        strcpy(e->file, "(demo integree)");
        e->builtin = 1;
        e->valid = n64_parse(demo_rom, demo_rom_size, demo_rom_size, ORDER_Z64, &e->info) == 0;
        strcpy(e->title, "Demo N64 (ROM maison)");
    }
    for (i = 0; dirs && dirs[i]; i++) plat_list_dir(dirs[i], scan_cb, L);
}

int rom_find_file(const RomLib *L, const char *file)
{
    int i;
    for (i = 0; i < L->count; i++) if (!strcasecmp(L->e[i].file, file)) return i;
    return -1;
}

static int subseq(const char *n, const char *h)
{
    while (*n && *h) { if (*n == *h) n++; h++; }
    return *n == 0;
}

static int score(const RomEntry *e, const char *q)
{
    char title[72], internal[32], file[120], region[40], tok[48];
    const char *p = q;
    int total = 0;
    text_fold(e->title, title, sizeof title);
    text_fold(e->info.internal, internal, sizeof internal);
    text_fold(e->file, file, sizeof file);
    text_fold(n64_region_name(e->info.region_letter), region, sizeof region);
    while (*p) {
        int n = 0, s = 0;
        char *hit;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < 47) tok[n++] = *p++;
        tok[n] = 0;
        if (!n) break;
        if (!strncmp(title, tok, n)) s = 100;
        else if ((hit = strstr(title, tok))) s = (hit[-1] == ' ' || hit[-1] == '-' || hit[-1] == ':') ? 80 : 60;
        if (!s && !strcmp(tok, e->info.code)) s = 90;
        if (!s && strstr(internal, tok)) s = 55;
        if (!s && strstr(region, tok)) s = 45;
        if (!s && strstr(file, tok)) s = 40;
        if (!s && n >= 3 && subseq(tok, title)) s = 10;
        if (!s) return 0;
        total += s;
    }
    return total ? total : 1;
}

static const RomLib *gL;
static int gsort, gscore[ROM_MAX];

static int cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b, d;
    if (gscore[ia] != gscore[ib]) return gscore[ib] - gscore[ia];
    if (gsort == RSORT_FAV && gL->fav[ia] != gL->fav[ib]) return gL->fav[ib] - gL->fav[ia];
    if (gsort == RSORT_RECENT) {
        int ra = gL->recent_rank[ia] ? gL->recent_rank[ia] : 10000;
        int rb = gL->recent_rank[ib] ? gL->recent_rank[ib] : 10000;
        if (ra != rb) return ra - rb;
    }
    d = strcasecmp(gL->e[ia].title, gL->e[ib].title);
    return d ? d : ia - ib;
}

void rom_filter(RomLib *L, const char *query, int category, int sort)
{
    char q[64];
    int i;
    text_fold(query ? query : "", q, sizeof q);
    L->view_count = 0;
    for (i = 0; i < L->count; i++) {
        char r = L->e[i].info.region_letter;
        int s = 1;
        if (category == RCAT_FAV && !L->fav[i]) continue;
        if (category == RCAT_RECENT && !L->recent_rank[i]) continue;
        if (category == RCAT_USA && r != 'E' && r != 'N') continue;
        if (category == RCAT_EUROPE && !n64_region_is_pal(r)) continue;
        if (category == RCAT_JAPAN && r != 'J') continue;
        if (q[0]) { s = score(&L->e[i], q); if (!s) continue; }
        gscore[i] = q[0] ? s : 0;
        L->view[L->view_count++] = i;
    }
    gL = L;
    gsort = category == RCAT_RECENT ? RSORT_RECENT : sort;
    qsort(L->view, L->view_count, sizeof(int), cmp);
}
