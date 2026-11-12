/* ui.c : interface de l'emulateur N64 (etapes 1-5 : bibliotheque, analyse des
 * ROMs, demarrage diagnostique, manette). */
#include "ui.h"
#include "gfx.h"
#include "roms.h"
#include "n64rom.h"
#include "n64pad.h"
#include "n64.h"
#include "jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BTN_X  "\x01"
#define BTN_O  "\x02"
#define BTN_SQ "\x03"
#define BTN_TR "\x04"

#define C_BG      RGB(12, 12, 18)
#define C_PANEL   RGB(26, 26, 36)
#define C_LINE    RGB(56, 56, 76)
#define C_TEXT    RGB(236, 236, 244)
#define C_DIM     RGB(144, 144, 166)
#define C_ACCENT  RGB(60, 150, 240)       /* bleu */
#define C_SEL     RGB(28, 50, 86)
#define C_OK      RGB(90, 210, 120)
#define C_WARN    RGB(240, 180, 60)
#define C_BAD     RGB(240, 90, 80)
#define VERSION   "ETAPE 6.0"

typedef struct {
    int aspect, size, region, expansion, sort;
    PadCfg pad;
    int frameskip;          /* -1 = auto, 0..3 */
    int cpi;                /* cycles par instruction 1..3 */
    int idle;               /* saut des boucles d'attente */
    int gfxq;               /* qualite 3D : 0 normale, 1 rapide, 2 tres rapide */
    int display;            /* 0 = RSX (rapide, lisse), 1 = logiciel */
    int jit;                /* recompilateur (si PS3MAPI est present) */
    int par;                /* dessin 3D sur le 2e fil du processeur */
} Config;

static Config cfg = { 0, 94, 0, 0, RSORT_NAME, { 12, 80, 0, 0 }, -1, 2, 1, 0, 0, 1, 1 };
static RomLib lib;
static int screen = SCR_SPLASH, options_from = SCR_LIBRARY, help_from = SCR_OPTIONS, pad_from = SCR_OPTIONS;
static int sel, scroll, category, frame_no, splash_t;
static char query[40];
static int kb_x, kb_y, opt_sel, pad_sel;
static char toast[96];
static int toast_t;
static int rep_dir[2], rep_t;

int ui_screen(void) { return screen; }
static void show_toast(const char *t) { snprintf(toast, sizeof toast, "%s", t); toast_t = 170; }

/* ---------------------------------------------------------------- config */

static void config_save(void)
{
    char path[300];
    FILE *f;
    int i, r;
    snprintf(path, sizeof path, "%s/config.ini", plat_data_dir());
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# N64 - configuration\naspect=%d\nsize=%d\nregion=%d\nexpansion=%d\nsort=%d\n"
               "deadzone=%d\nrange=%d\nz_r2=%d\nc_face=%d\nframeskip=%d\ncpi=%d\nidle=%d\ngfxq=%d\ndisplay=%d\njit=%d\npar=%d\n",
            cfg.aspect, cfg.size, cfg.region, cfg.expansion, cfg.sort,
            cfg.pad.deadzone, cfg.pad.range, cfg.pad.z_on_r2, cfg.pad.c_face, cfg.frameskip, cfg.cpi, cfg.idle, cfg.gfxq, cfg.display, cfg.jit, cfg.par);
    for (i = 0; i < lib.count; i++) if (lib.fav[i]) fprintf(f, "fav=%s\n", lib.e[i].file);
    for (r = 1; r <= 20; r++)
        for (i = 0; i < lib.count; i++) if (lib.recent_rank[i] == r) fprintf(f, "recent=%s\n", lib.e[i].file);
    fclose(f);
}

static int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

static void config_load(void)
{
    char path[300], line[300];
    FILE *f;
    int rank = 0;
    snprintf(path, sizeof path, "%s/config.ini", plat_data_dir());
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl, *val;
        int v;
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        val = eq + 1;
        if ((nl = strpbrk(val, "\r\n"))) *nl = 0;
        v = atoi(val);
        if (!strcmp(line, "aspect")) cfg.aspect = v % 2;
        else if (!strcmp(line, "size")) cfg.size = clampi(v, 80, 100);
        else if (!strcmp(line, "region")) cfg.region = v % 3;
        else if (!strcmp(line, "expansion")) cfg.expansion = v % 3;
        else if (!strcmp(line, "sort")) cfg.sort = v % RSORT_COUNT;
        else if (!strcmp(line, "deadzone")) cfg.pad.deadzone = clampi(v, 0, 30);
        else if (!strcmp(line, "range")) cfg.pad.range = clampi(v, 60, 85);
        else if (!strcmp(line, "z_r2")) cfg.pad.z_on_r2 = !!v;
        else if (!strcmp(line, "c_face")) cfg.pad.c_face = !!v;
        else if (!strcmp(line, "frameskip")) cfg.frameskip = clampi(v, -1, 3);
        else if (!strcmp(line, "cpi")) cfg.cpi = clampi(v, 1, 3);
        else if (!strcmp(line, "idle")) cfg.idle = !!v;
        else if (!strcmp(line, "gfxq")) cfg.gfxq = clampi(v, 0, 2);
        else if (!strcmp(line, "display")) cfg.display = !!v;
        else if (!strcmp(line, "jit")) cfg.jit = !!v;
        else if (!strcmp(line, "par")) cfg.par = !!v;
        else if (!strcmp(line, "fav")) { int i = rom_find_file(&lib, val); if (i >= 0) lib.fav[i] = 1; }
        else if (!strcmp(line, "recent")) { int i = rom_find_file(&lib, val); if (i >= 0 && rank < 20) lib.recent_rank[i] = ++rank; }
    }
    fclose(f);
}

static void mark_recent(int idx)
{
    int i, old = lib.recent_rank[idx];
    for (i = 0; i < lib.count; i++)
        if (lib.recent_rank[i] && (!old || lib.recent_rank[i] < old)) lib.recent_rank[i]++;
    lib.recent_rank[idx] = 1;
    for (i = 0; i < lib.count; i++) if (lib.recent_rank[i] > 20) lib.recent_rank[i] = 0;
}

/* ---------------------------------------------------------------- entrees */

static Pad merge(const Pad p[2])
{
    Pad m = p[0];
    m.held |= p[1].held;
    m.pressed |= p[1].pressed;
    if (fabsf(p[1].lx) > fabsf(m.lx)) m.lx = p[1].lx;
    if (fabsf(p[1].ly) > fabsf(m.ly)) m.ly = p[1].ly;
    return m;
}

static void nav(const Pad *p, int *dx, int *dy)
{
    int x = 0, y = 0;
    if ((p->held & B_LEFT) || p->lx < -0.6f) x = -1;
    if ((p->held & B_RIGHT) || p->lx > 0.6f) x = 1;
    if ((p->held & B_UP) || p->ly < -0.6f) y = -1;
    if ((p->held & B_DOWN) || p->ly > 0.6f) y = 1;
    *dx = *dy = 0;
    if (x != rep_dir[0] || y != rep_dir[1]) { rep_dir[0] = x; rep_dir[1] = y; rep_t = 0; *dx = x; *dy = y; return; }
    if (x || y) { rep_t++; if (rep_t > 16 && (rep_t % 4) == 0) { *dx = x; *dy = y; } }
}

/* ---------------------------------------------------------------- dessin commun */

static void disc(float cx, float cy, float r, uint32_t col)
{
    int i, n = (int)(r * 2);
    for (i = 0; i < n; i++) {
        float y = -r + (i + 0.5f) * 2 * r / n, w = sqrtf(r * r - y * y);
        gfx_rect(cx - w, cy + y - r / n, 2 * w, 2 * r / n + 0.5f, col);
    }
}

static void ring(float cx, float cy, float r, float t, uint32_t col)
{
    int i;
    for (i = 0; i < 64; i++) {
        float a0 = i * 6.2831853f / 64, a1 = (i + 1) * 6.2831853f / 64;
        gfx_line(cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, t, col);
    }
}

static void footer(const char *t)
{
    gfx_rect(0, 676, REF_W, 44, RGB(18, 18, 26));
    gfx_rect(0, 676, REF_W, 2, C_LINE);
    gfx_text(60, 690, t, 2, C_DIM);
}

static void draw_toast(void)
{
    float w;
    if (toast_t <= 0) return;
    toast_t--;
    w = gfx_text_width(toast, 2) + 40;
    gfx_rect((REF_W - w) / 2, 570, w, 40, C_ACCENT);
    gfx_text((REF_W - w) / 2 + 20, 583, toast, 2, RGB(255, 255, 255));
}

static void header(const char *r1, const char *r2)
{
    static const uint32_t bars[4] = { 0xE03030, 0xF0C020, 0x30A040, 0x3080E0 };
    int i;
    gfx_rect(0, 0, REF_W, 92, RGB(8, 8, 12));
    for (i = 0; i < 4; i++) gfx_rect(i * REF_W / 4.0f, 86, REF_W / 4.0f, 6, bars[i]);
    gfx_text(60, 24, "N64", 5, RGB(250, 250, 250));
    gfx_text(60 + gfx_text_width("N64 ", 5), 42, "EMULATEUR PS3  -  " VERSION, 2, C_DIM);
    if (r1) gfx_text(REF_W - 60 - gfx_text_width(r1, 2), 26, r1, 2, C_DIM);
    if (r2) gfx_text(REF_W - 60 - gfx_text_width(r2, 2), 50, r2, 2, C_DIM);
}

/* cube 3D en fil de fer (ecran titre et fiche) */
static void cube(float cx, float cy, float size, float t, float lw)
{
    static const float v[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
    static const int e[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
    static const uint32_t col[4] = { 0xE03030, 0xF0C020, 0x30A040, 0x3080E0 };
    float p[8][2], ca = cosf(t), sa = sinf(t), cb = cosf(t * 0.7f), sb = sinf(t * 0.7f);
    int i;
    for (i = 0; i < 8; i++) {
        float x = v[i][0], y = v[i][1], z = v[i][2];
        float x1 = x * ca - z * sa, z1 = x * sa + z * ca;
        float y1 = y * cb - z1 * sb, z2 = y * sb + z1 * cb;
        float k = 4.0f / (z2 + 5.0f);
        p[i][0] = cx + x1 * size * k;
        p[i][1] = cy + y1 * size * k;
    }
    for (i = 0; i < 12; i++) gfx_line(p[e[i][0]][0], p[e[i][0]][1], p[e[i][1]][0], p[e[i][1]][1], lw, col[i & 3]);
}

static const char *display_title(const RomEntry *e) { return e->title; }

/* ---------------------------------------------------------------- bibliotheque */

static void refilter(void)
{
    int keep = (lib.view_count && sel < lib.view_count) ? lib.view[sel] : -1, i;
    rom_filter(&lib, query, category, cfg.sort);
    sel = 0;
    if (!query[0]) for (i = 0; i < lib.view_count; i++) if (lib.view[i] == keep) sel = i;
    scroll = sel - 5 < 0 ? 0 : sel - 5;
}

static void start_boot(int idx);

static void cart_art(float x, float y, const RomEntry *e)
{
    char t[24];
    /* cartouche grise stylisee (dessin original) */
    gfx_rect(x, y + 20, 300, 170, RGB(96, 98, 106));
    gfx_rect(x + 20, y, 260, 30, RGB(96, 98, 106));
    gfx_rect(x + 30, y + 40, 240, 110, RGB(225, 225, 232));
    gfx_rect(x + 30, y + 40, 240, 22, C_ACCENT);
    gfx_text(x + 40, y + 46, e->info.code, 2, RGB(255, 255, 255));
    snprintf(t, sizeof t, "%.16s", e->info.internal[0] ? e->info.internal : "?");
    gfx_text(x + 40, y + 80, t, 2, RGB(30, 30, 40));
    gfx_rect(x + 10, y + 190, 280, 10, RGB(60, 62, 70));
}

static void draw_library(int searching)
{
    static const char *sorts[] = { "NOM", "FAVORIS", "RECENTS" };
    int rows_vis = searching ? 4 : 11, i;
    char b[200], b2[64];
    float x;
    const RomEntry *g = lib.view_count ? &lib.e[lib.view[sel]] : NULL;

    gfx_clear(C_BG);
    snprintf(b, sizeof b, "%d ROM(S)", lib.count);
    snprintf(b2, sizeof b2, "TRI : %s", sorts[cfg.sort]);
    header(b, b2);

    gfx_rect(60, 110, 640, 42, searching ? RGB(24, 40, 70) : C_PANEL);
    gfx_frame(60, 110, 640, 42, 2, searching ? C_ACCENT : C_LINE);
    gfx_text(74, 122, BTN_TR, 3, RGB(80, 200, 160));
    if (query[0] || searching) {
        snprintf(b, sizeof b, "%s%s", query, (searching && (frame_no / 20) & 1) ? "_" : "");
        gfx_text(110, 122, b, 3, C_TEXT);
        snprintf(b, sizeof b, "%d RESULTAT%s", lib.view_count, lib.view_count > 1 ? "S" : "");
        gfx_text(690 - gfx_text_width(b, 2), 126, b, 2, C_DIM);
    } else gfx_text(110, 125, "RECHERCHER : TITRE, CODE (NSME), REGION...", 2, C_DIM);

    x = 60;
    for (i = 0; rom_categories[i]; i++) {
        float w = gfx_text_width(rom_categories[i], 2) + 18;
        if (i == category) gfx_rect(x, 166, w, 30, C_ACCENT);
        else gfx_frame(x, 166, w, 30, 2, C_LINE);
        gfx_text(x + 9, 174, rom_categories[i], 2, i == category ? RGB(255, 255, 255) : C_DIM);
        x += w + 5;
    }

    if (sel < scroll) scroll = sel;
    if (sel >= scroll + rows_vis) scroll = sel - rows_vis + 1;
    for (i = 0; i < 11 && scroll + i < lib.view_count; i++) {
        int gi = lib.view[scroll + i];
        const RomEntry *e = &lib.e[gi];
        float y = 212 + i * 41;
        int on = scroll + i == sel;
        char t[40];
        gfx_rect(60, y, 640, 37, on ? C_SEL : (i & 1 ? RGB(18, 18, 26) : RGB(22, 22, 31)));
        if (on) gfx_rect(60, y, 5, 37, C_ACCENT);
        snprintf(b, sizeof b, "%uM", (unsigned)((e->info.size + (1 << 20) - 1) >> 20));
        gfx_rect(76, y + 5, 60, 26, e->info.known ? RGB(40, 90, 160) : RGB(70, 70, 86));
        gfx_text(106 - gfx_text_width(b, 2) / 2, y + 11, b, 2, C_TEXT);
        snprintf(t, sizeof t, "%s", display_title(e));
        if (strlen(display_title(e)) > 30) strcpy(t + 27, "...");
        gfx_text(150, y + 11, t, 2, on ? RGB(255, 255, 255) : C_TEXT);
        if (lib.fav[gi]) gfx_text(150 + gfx_text_width(t, 2) + 10, y + 11, "*", 2, C_WARN);
        gfx_text(684 - gfx_text_width(e->info.code, 2), y + 11, e->info.code, 2,
                 n64_region_is_pal(e->info.region_letter) ? RGB(120, 180, 240) : C_DIM);
    }
    if (!lib.count) {
        gfx_text(80, 240, "AUCUNE ROM N64 TROUVEE.", 2, C_DIM);
        gfx_text(80, 270, "COPIEZ VOS .Z64 / .N64 / .V64 DANS /DEV_USB000/N64/", 2, C_DIM);
        gfx_text(80, 294, "PUIS START > OPTIONS > RECHARGER LES ROMS.", 2, C_DIM);
    } else if (!lib.view_count) {
        gfx_text(80, 240, "AUCUN JEU NE CORRESPOND.", 2, C_DIM);
        gfx_text(80, 270, BTN_O " POUR EFFACER LA RECHERCHE", 2, C_DIM);
    }
    if (lib.view_count > rows_vis) {
        float h = 450.0f * rows_vis / lib.view_count, yy = 212 + 450.0f * scroll / lib.view_count;
        gfx_rect(708, 212, 4, 450, C_LINE);
        gfx_rect(706, yy, 8, h, C_ACCENT);
    }

    gfx_rect(740, 110, 480, 556, C_PANEL);
    if (g) {
        const N64Info *n = &g->info;
        float y = 336;
        cart_art(830, 120, g);
        gfx_wrap(756, y, 450, display_title(g), 3, RGB(255, 255, 255), 2); y += 62;
        snprintf(b, sizeof b, "%s  -  %s  V1.%d", n->code, n64_region_name(n->region_letter), n->version);
        gfx_text(756, y, b, 2, C_ACCENT); y += 28;
        snprintf(b, sizeof b, "TITRE INTERNE : %s", n->internal[0] ? n->internal : "-");
        gfx_text(756, y, b, 2, C_TEXT); y += 26;
        snprintf(b, sizeof b, "TAILLE : %u MO   FORMAT : %s", (unsigned)(n->size >> 20), n64_order_names[n->order]);
        gfx_text(756, y, b, 1.5f, C_DIM); y += 22;
        if (n->cic) snprintf(b, sizeof b, "PUCE : CIC-NUS-%d", n->cic);
        else snprintf(b, sizeof b, "PUCE : INCONNUE (HOMEBREW ?)");
        gfx_text(756, y, b, 2, n->cic ? C_TEXT : C_WARN); y += 26;
        if (n->checksum == 1) gfx_text(756, y, "SOMME DE CONTROLE : OK", 2, C_OK);
        else if (n->checksum == 0) gfx_text(756, y, "SOMME DE CONTROLE : DIFFERENTE (ROM MODIFIEE ?)", 2, C_BAD);
        else gfx_text(756, y, "SOMME DE CONTROLE : NON VERIFIABLE", 2, C_WARN);
        y += 26;
        if (n->known) {
            snprintf(b, sizeof b, "SAUVEGARDE : %s   %d JOUEUR%s", n64_save_names[n->save], n->players, n->players > 1 ? "S" : "");
            gfx_text(756, y, b, 2, C_TEXT); y += 26;
            if (n->expansion == 1) { gfx_text(756, y, "EXPANSION PAK NECESSAIRE", 2, C_WARN); y += 24; }
            else if (n->expansion == 2) { gfx_text(756, y, "EXPANSION PAK CONSEILLE", 2, C_DIM); y += 24; }
        }
    }
    if (!searching)
        footer(BTN_X " DEMARRER   " BTN_TR " RECHERCHE   " BTN_SQ " FAVORI   L1/R1 FILTRE   SELECT TRI   START OPTIONS");
}

static void update_library(const Pad *m)
{
    int dx, dy;
    nav(m, &dx, &dy);
    if (dy && lib.view_count) sel = (sel + dy + lib.view_count) % lib.view_count;
    if (dx && lib.view_count) {
        sel += dx * 11;
        if (sel < 0) sel = 0;
        if (sel >= lib.view_count) sel = lib.view_count - 1;
    }
    if (m->pressed & B_L1) { category = (category + RCAT_COUNT - 1) % RCAT_COUNT; refilter(); }
    if (m->pressed & B_R1) { category = (category + 1) % RCAT_COUNT; refilter(); }
    if (m->pressed & B_SELECT) { cfg.sort = (cfg.sort + 1) % RSORT_COUNT; refilter(); config_save(); }
    if (m->pressed & B_TRIANGLE) { screen = SCR_SEARCH; kb_x = kb_y = 0; }
    if ((m->pressed & B_CIRCLE) && (query[0] || category)) { query[0] = 0; category = 0; refilter(); }
    if ((m->pressed & B_SQUARE) && lib.view_count) {
        int gi = lib.view[sel];
        lib.fav[gi] ^= 1;
        show_toast(lib.fav[gi] ? "AJOUTE AUX FAVORIS" : "RETIRE DES FAVORIS");
        config_save();
        if (cfg.sort == RSORT_FAV || category == RCAT_FAV) refilter();
    }
    if ((m->pressed & B_CROSS) && lib.view_count) start_boot(lib.view[sel]);
    if (m->pressed & B_START) { options_from = SCR_LIBRARY; opt_sel = 0; screen = SCR_OPTIONS; }
}

/* ---------------------------------------------------------------- recherche */

static const char *kb_rows[] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789:'-." };
static const char *kb_special[] = { "ESPACE", "EFFACER", "VIDER", "OK" };

static void draw_search(void)
{
    int r, c;
    float x0 = 60, y0 = 418;
    draw_library(1);
    gfx_rect_blend(0, 376, 730, 344, RGB(0, 0, 0), 170);
    gfx_rect(x0, y0 - 34, 640, 288, RGB(26, 28, 40));
    gfx_frame(x0, y0 - 34, 640, 288, 2, C_ACCENT);
    gfx_text(x0 + 20, y0 - 23, "CLAVIER DE RECHERCHE", 2, C_ACCENT);
    for (r = 0; r < 4; r++)
        for (c = 0; c < 10; c++) {
            char k[2] = { kb_rows[r][c], 0 };
            float x = x0 + 20 + c * 60, y = y0 + r * 46;
            int on = kb_y == r && kb_x == c;
            gfx_rect(x, y, 54, 41, on ? C_ACCENT : RGB(46, 48, 64));
            gfx_text(x + 20, y + 10, k, 3, C_TEXT);
        }
    for (c = 0; c < 4; c++) {
        float x = x0 + 20 + c * 150, y = y0 + 4 * 46;
        int on = kb_y == 4 && kb_x == c;
        gfx_rect(x, y, 144, 41, on ? C_ACCENT : RGB(46, 48, 64));
        gfx_text(x + (144 - gfx_text_width(kb_special[c], 2)) / 2, y + 14, kb_special[c], 2, C_TEXT);
    }
    footer(BTN_X " TAPER   " BTN_O " EFFACER   " BTN_SQ " ESPACE   " BTN_TR "/START VALIDER   L1/R1 RESULTAT");
}

static void kb_type(char ch) { size_t n = strlen(query); if (n < sizeof(query) - 1) { query[n] = ch; query[n + 1] = 0; refilter(); } }
static void kb_back(void) { size_t n = strlen(query); if (n) { query[n - 1] = 0; refilter(); } }

static void update_search(const Pad *m)
{
    int dx, dy, cols;
    nav(m, &dx, &dy);
    if (dy) {
        int old = kb_y;
        kb_y = (kb_y + dy + 5) % 5;
        if (kb_y == 4 && old != 4) kb_x = kb_x * 4 / 10;
        else if (old == 4 && kb_y != 4) kb_x = kb_x * 10 / 4 + 1;
    }
    cols = kb_y == 4 ? 4 : 10;
    if (dx) kb_x = (kb_x + dx + cols) % cols;
    if (kb_x >= cols) kb_x = cols - 1;
    if (m->pressed & B_CROSS) {
        if (kb_y < 4) kb_type(kb_rows[kb_y][kb_x]);
        else if (kb_x == 0) kb_type(' ');
        else if (kb_x == 1) kb_back();
        else if (kb_x == 2) { query[0] = 0; refilter(); }
        else screen = SCR_LIBRARY;
    }
    if (m->pressed & B_CIRCLE) kb_back();
    if (m->pressed & B_SQUARE) kb_type(' ');
    if ((m->pressed & B_R1) && lib.view_count) sel = (sel + 1) % lib.view_count;
    if ((m->pressed & B_L1) && lib.view_count) sel = (sel + lib.view_count - 1) % lib.view_count;
    if (m->pressed & (B_TRIANGLE | B_START)) screen = SCR_LIBRARY;
}

/* ---------------------------------------------------------------- demarrage (diagnostic) */

#define CHUNK (4u << 20)
static struct {
    int idx, phase;         /* 0 lecture, 1 conversion, 2 CRC32, 3 termine, -1 erreur */
    FILE *f;
    uint8_t *rom;
    uint32_t size, done;
    uint32_t crc;
    char err[80];
} boot;

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t i, c;
    int k;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static void boot_free(void)
{
    if (boot.f) fclose(boot.f);
    free(boot.rom);
    memset(&boot, 0, sizeof boot);
}

static void start_boot(int idx)
{
    const RomEntry *e = &lib.e[idx];
    boot_free();
    boot.idx = idx;
    boot.size = e->info.size;
    boot.crc = 0xFFFFFFFFu;
    if (e->builtin) {
        boot.rom = (uint8_t *)malloc(demo_rom_size);
        if (boot.rom) memcpy(boot.rom, demo_rom, demo_rom_size);
        boot.size = demo_rom_size;
        boot.done = 0;
        boot.phase = 1;
        mark_recent(idx);
        screen = SCR_BOOT;
        return;
    }
    boot.f = fopen(e->path, "rb");
    boot.rom = (uint8_t *)malloc(boot.size ? boot.size : 1);
    if (!boot.f || !boot.rom) {
        boot.phase = -1;
        snprintf(boot.err, sizeof boot.err, !boot.f ? "IMPOSSIBLE D'OUVRIR LE FICHIER" : "MEMOIRE INSUFFISANTE");
    }
    mark_recent(idx);
    config_save();
    screen = SCR_BOOT;
}

static void update_boot_work(void)
{
    if (boot.phase == 0) {
        uint32_t n = boot.size - boot.done < CHUNK ? boot.size - boot.done : CHUNK;
        if (n && fread(boot.rom + boot.done, 1, n, boot.f) != n) {
            boot.phase = -1; snprintf(boot.err, sizeof boot.err, "ERREUR DE LECTURE"); return;
        }
        boot.done += n;
        if (boot.done >= boot.size) { fclose(boot.f); boot.f = NULL; boot.phase = 1; boot.done = 0; }
    } else if (boot.phase == 1) {
        n64_to_big_endian(boot.rom, boot.size, lib.e[boot.idx].info.order);
        boot.phase = 2;
    } else if (boot.phase == 2) {
        uint32_t end = boot.done + CHUNK > boot.size ? boot.size : boot.done + CHUNK, i, c = boot.crc;
        for (i = boot.done; i < end; i++) c = crc_table[(c ^ boot.rom[i]) & 0xFF] ^ (c >> 8);
        boot.crc = c;
        boot.done = end;
        if (boot.done >= boot.size) { boot.crc = ~boot.crc; boot.phase = 3; }
    }
}

static void check_line(float y, const char *label, const char *value, int state)
{
    uint32_t col = state == 1 ? C_OK : state == 0 ? C_BAD : state == 2 ? C_WARN : C_DIM;
    const char *tag = state == 1 ? "OK" : state == 0 ? "!!" : state == 2 ? "??" : "..";
    gfx_rect(100, y, 40, 30, col);
    gfx_text(120 - gfx_text_width(tag, 2) / 2, y + 8, tag, 2, RGB(16, 16, 20));
    gfx_text(160, y + 8, label, 2, C_TEXT);
    gfx_text(640, y + 8, value, 2, col == C_DIM ? C_DIM : C_TEXT);
}

static void draw_boot(void)
{
    const RomEntry *e = &lib.e[boot.idx];
    const N64Info *n = &e->info;
    char b[120];
    float y = 150;
    int region_pal = cfg.region == 2 || (cfg.region == 0 && n64_region_is_pal(n->region_letter));
    int exp = cfg.expansion == 1 || (cfg.expansion == 0 && n->expansion);

    gfx_clear(C_BG);
    header("DEMARRAGE", NULL);
    gfx_wrap(100, 110, 1080, e->title, 3, RGB(255, 255, 255), 1);

    if (boot.phase == -1) {
        check_line(y, "LECTURE DE LA ROM", boot.err, 0);
        footer(BTN_O " RETOUR");
        return;
    }
    if (boot.size < (1u << 20)) snprintf(b, sizeof b, "%u KO", (unsigned)(boot.size >> 10));
    else snprintf(b, sizeof b, "%u / %u MO", (unsigned)((boot.phase == 0 ? boot.done : boot.size) >> 20), (unsigned)(boot.size >> 20));
    check_line(y, "LECTURE DE LA ROM", b, boot.phase > 0 ? 1 : 3); y += 40;
    if (boot.phase == 0) {
        gfx_rect(160, y, 900, 16, C_LINE);
        gfx_rect(160, y, 900.0f * boot.done / (boot.size ? boot.size : 1), 16, C_ACCENT);
        footer("CHARGEMENT...");
        return;
    }
    check_line(y, "ORDRE DES OCTETS", n64_order_names[n->order], 1); y += 40;
    if (boot.phase < 3) {
        snprintf(b, sizeof b, "%u %%", (unsigned)(100.0 * boot.done / (boot.size ? boot.size : 1)));
        check_line(y, "EMPREINTE CRC32 DU FICHIER", b, 3);
        footer("CALCUL...");
        return;
    }
    snprintf(b, sizeof b, "%08X", (unsigned)boot.crc);
    check_line(y, "EMPREINTE CRC32 DU FICHIER", b, 1); y += 40;
    snprintf(b, sizeof b, "%s (%s)", n->code, n64_region_name(n->region_letter));
    check_line(y, "CODE / REGION", b, 1); y += 40;
    if (n->cic) snprintf(b, sizeof b, "CIC-NUS-%d", n->cic); else snprintf(b, sizeof b, "INCONNUE");
    check_line(y, "PUCE DE SECURITE", b, n->cic ? 1 : 2); y += 40;
    snprintf(b, sizeof b, "%08X %08X", (unsigned)n->crc_hdr[0], (unsigned)n->crc_hdr[1]);
    check_line(y, "SOMME DE CONTROLE IPL3", b, n->checksum == 1 ? 1 : n->checksum == 0 ? 0 : 2); y += 40;
    snprintf(b, sizeof b, "%08X", (unsigned)n->entry);
    check_line(y, "ADRESSE DE DEMARRAGE", b, (n->entry & 0xF0000000u) == 0x80000000u ? 1 : 2); y += 40;
    check_line(y, "SAUVEGARDE PREVUE", n->known ? n64_save_names[n->save] : "INCONNUE (EEPROM 4 KBIT PAR DEFAUT)", n->known ? 1 : 2); y += 40;
    snprintf(b, sizeof b, "%s - %s", region_pal ? "PAL 50 HZ" : "NTSC 60 HZ", exp ? "8 MO (EXPANSION PAK)" : "4 MO");
    check_line(y, "VIDEO / MEMOIRE", b, 1); y += 40;
    check_line(y, "PROCESSEUR VR4300 (INTERPRETEUR)", "PRET", 1); y += 52;

    gfx_rect(100, y, 1080, 70, C_PANEL);
    gfx_frame(100, y, 1080, 70, 2, C_ACCENT);
    gfx_text(120, y + 14, "LA ROM EST CHARGEE ET PRETE.", 2, C_OK);
    gfx_text(120, y + 40, "GRAPHISMES ET SON EN LOGICIEL : LENT AVANT L'ETAPE 5 (RECOMPILATEUR).", 2, C_TEXT);
    footer(BTN_X " LANCER L'EMULATION   " BTN_O " RETOUR A LA BIBLIOTHEQUE");
}

static void emu_start(void);

static void update_boot(const Pad *m)
{
    if (boot.phase >= 0 && boot.phase < 3) update_boot_work();
    if ((m->pressed & B_CROSS) && boot.phase == 3) { emu_start(); return; }
    if ((m->pressed & B_CIRCLE) && (boot.phase == 3 || boot.phase == -1 || boot.phase == 0)) {
        boot_free();
        screen = SCR_LIBRARY;
    }
}

/* ---------------------------------------------------------------- emulation */

static N64 emu;
static int emu_on, emu_info = 1, run_sel, emu_sound = 1, emu_slot, emu_save_type;
static uint64_t emu_t0, emu_frames0, speed_t;
static float emu_speed;
static uint8_t *emu_rom;
static uint32_t emu_rom_size;
static char emu_name[120];

/* ---- sauvegardes de cartouche (EEPROM / SRAM) : USRDIR/saves/<rom>.eep|.sra */
static void save_path(char *out, size_t n, const char *ext)
{
    snprintf(out, n, "%s/saves/%s.%s", plat_data_dir(), emu_name, ext);
}

static void cart_saves_load(void)
{
    char path[400];
    FILE *f;
    if (emu.eeprom_type) {
        save_path(path, sizeof path, "eep");
        if ((f = fopen(path, "rb")) != NULL) { if (fread(emu.eeprom, 1, emu.eeprom_type == 2 ? 2048 : 512, f)) {} fclose(f); }
    }
    if (emu.sram_enabled) {
        save_path(path, sizeof path, "sra");
        if ((f = fopen(path, "rb")) != NULL) { if (fread(emu.sram, 1, sizeof emu.sram, f)) {} fclose(f); }
    }
    emu.eeprom_dirty = emu.sram_dirty = 0;
}

static void cart_saves_flush(void)
{
    char path[400];
    FILE *f;
    if (!emu_on) return;
    if (emu.eeprom_dirty && emu.eeprom_type) {
        save_path(path, sizeof path, "eep");
        if ((f = fopen(path, "wb")) != NULL) { fwrite(emu.eeprom, 1, emu.eeprom_type == 2 ? 2048 : 512, f); fclose(f); }
        emu.eeprom_dirty = 0;
    }
    if (emu.sram_dirty && emu.sram_enabled) {
        save_path(path, sizeof path, "sra");
        if ((f = fopen(path, "wb")) != NULL) { fwrite(emu.sram, 1, sizeof emu.sram, f); fclose(f); }
        emu.sram_dirty = 0;
    }
}

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/states/%s.st%d", plat_data_dir(), emu_name, emu_slot);
}

static void audio_cb(void *ctx, const int16_t *st, int frames)
{
    (void)ctx;
    if (emu_sound) plat_audio_push(st, frames);
}

static void emu_stop(void)
{
    if (!emu_on) return;
    cart_saves_flush();
    n64_free(&emu);
    free(emu_rom);
    emu_rom = NULL;
    emu_on = 0;
}

static int auto_skip, auto_good;
static uint64_t prof_emu, prof_other, prof_last, last_draw;
static int draw_us;
static int pct_cpu, pct_gfx, pct_snd, pct_ui;

/* recompilateur : -1 indisponible, 0 coupe, 1 actif ; jit_msg = version PS3MAPI ou raison */
static int jit_status;
static const char *jit_msg = "";

static void jit_apply(void)
{
    if (!emu_on) return;
    if (cfg.jit && !emu.cpu.jit) jit_status = plat_jit_attach(&emu.cpu, &jit_msg) == 0 ? 1 : -1;
    else if (!cfg.jit && emu.cpu.jit) { jit_free(&emu.cpu); jit_status = 0; }
    else if (!cfg.jit) jit_status = 0;
    n64_set_gfx_worker(&emu, cfg.par ? (N64Worker *)plat_gfx_worker() : NULL);
}

static void emu_setup(void)
{
    emu.audio_out = audio_cb;
    emu.clock_us = plat_time_us;
    prof_emu = prof_other = prof_last = 0;
    emu.cpu.cpi = (unsigned)cfg.cpi;
    emu.cpu.idle_skip = cfg.idle;
    emu.rdp.fast = cfg.gfxq;
    auto_skip = 0; auto_good = 0;
    emu.frameskip = cfg.frameskip >= 0 ? cfg.frameskip : 0;
    cart_saves_load();
    n64_boot_hle(&emu);
    jit_apply();
}

static void emu_start(void)
{
    const RomEntry *e = &lib.e[boot.idx];
    const N64Info *n = &e->info;
    int pal = cfg.region == 2 || (cfg.region == 0 && n64_region_is_pal(n->region_letter));
    int exp = cfg.expansion == 1 || (cfg.expansion == 0 && n->expansion);
    char *dot;
    emu_stop();
    emu_rom = boot.rom;                 /* la ROM chargee passe a l'emulateur */
    emu_rom_size = boot.size;
    boot.rom = NULL;
    boot_free();
    emu_save_type = n->known ? n->save : SAVE_EEPROM4K;
    snprintf(emu_name, sizeof emu_name, "%s", e->builtin ? "demo" : e->file);
    if ((dot = strrchr(emu_name, '.')) != NULL) *dot = 0;
    if (n64_init(&emu, emu_rom, emu_rom_size, n->cic, pal, exp, emu_save_type) != 0) {
        free(emu_rom); emu_rom = NULL;
        show_toast("MEMOIRE INSUFFISANTE");
        screen = SCR_LIBRARY;
        return;
    }
    emu_on = 1;
    emu_setup();      /* (emu_on doit deja valoir 1 pour le recompilateur) */
    emu_t0 = plat_time_us();
    emu_frames0 = 0;
    speed_t = emu_t0;
    emu_speed = 0;
    screen = SCR_RUN;
}

static void emu_step(const Pad pads[2])
{
    int i;
    uint64_t t0 = plat_time_us(), now;
    for (i = 0; i < 2; i++) {
        N64Pad p;
        n64pad_convert(&pads[i], &cfg.pad, &p);
        emu.pad_buttons[i] = p.buttons;
        emu.pad_x[i] = p.x;
        emu.pad_y[i] = p.y;
        emu.pad_connected[i] = i == 0 || pads[i].connected;
    }
    /* emule jusqu'a la prochaine image REELLEMENT dessinee (les images sautees ne
     * redessinent pas l'ecran PS3) ; 120 ms au plus pour garder la main */
    {
        uint64_t te = plat_time_us();
        if (prof_last) prof_other += te - prof_last;       /* ecran + attente de l'image precedente */
        emu.fb_new = 0;
        do {
            n64_run(&emu, 16);
            now = plat_time_us();
        } while (!emu.fb_new && now - t0 < 120000);
        prof_emu += now - te;
        prof_last = now;
    }
    now = plat_time_us();
    if (now - speed_t >= 1000000) {
        double fps = (double)(emu.frames - emu_frames0) * 1e6 / (double)(now - speed_t);
        uint64_t tot = prof_emu + prof_other;
        emu_speed = (float)(fps * 100.0 / (emu.pal ? 50.0 : 60.0));
        emu_frames0 = emu.frames;
        speed_t = now;
        if (tot) {
            /* avec le 2e fil, la 3D ne prend plus de temps au fil principal : son
             * pourcentage est alors l'occupation du 2e fil */
            uint64_t on_main = emu.gfx_worker ? emu.t_audio : emu.t_gfx + emu.t_audio;
            uint64_t cpu = prof_emu > on_main ? prof_emu - on_main : 0;
            pct_cpu = (int)(cpu * 100 / tot); pct_gfx = (int)(emu.t_gfx * 100 / tot);
            pct_snd = (int)(emu.t_audio * 100 / tot); pct_ui = (int)(prof_other * 100 / tot);
        }
        prof_emu = prof_other = 0; emu.t_gfx = emu.t_audio = 0;
        cart_saves_flush();                 /* au plus une ecriture par seconde */
        if (cfg.frameskip < 0) {            /* saut d'images automatique */
            if (emu_speed < 92 && auto_skip < 3) { auto_skip++; auto_good = 0; }
            else if (emu_speed >= 99 && auto_skip > 0 && ++auto_good >= 3) { auto_skip--; auto_good = 0; }
            emu.frameskip = auto_skip;
        }
    }
}

static UiRun run_info;
const UiRun *ui_run_info(void) { return &run_info; }

static int run_direct(void) { return plat_rsx_blit() && !cfg.display; }

static void draw_run(void)
{
    char b[120];
    float h = REF_H * cfg.size / 100.0f, w = cfg.aspect ? REF_W * cfg.size / 100.0f : h * 4 / 3;
    float x = (REF_W - w) / 2, y = (REF_H - h) / 2;
    if (run_direct() && emu.fb_valid) {
        /* le RSX agrandit l'image : le canevas ne contient que le noir et le texte */
        gfx_clear(RGB(0, 0, 0));
        run_info.px = emu.fb; run_info.w = emu.fb_w; run_info.h = emu.fb_h;
        gfx_rect_to_px(x, y, w, h, run_info.dst);
        run_info.canvas_dirty = 1;
        run_info.novl = 0;
    } else if (emu.fb_valid) {
        /* seulement les bords (l'image du jeu recouvre le reste) : 3 Mo de moins a ecrire */
        gfx_rect(0, 0, REF_W, y + 1, RGB(0, 0, 0));
        gfx_rect(0, y + h - 1, REF_W, REF_H - (y + h) + 2, RGB(0, 0, 0));
        gfx_rect(0, y, x + 1, h, RGB(0, 0, 0));
        gfx_rect(x + w - 1, y, REF_W - (x + w) + 2, h, RGB(0, 0, 0));
        gfx_blit_scaled(emu.fb, emu.fb_w, emu.fb_h, x, y, w, h, 0);
    } else gfx_clear(RGB(0, 0, 0));
    if (emu_info || !emu.fb_valid) {
        float py = 20;
        if (run_direct() && emu.fb_valid) {
            gfx_rect(20, 14, 640, 352, RGB(0, 0, 0));            /* opaque : pose par le RSX */
            gfx_rect_to_px(20, 14, 640, 352, run_info.ovl[run_info.novl++]);
        } else gfx_rect_blend(20, 14, 640, emu.fb_valid ? 352 : 378, RGB(0, 0, 0), 190);
        if (emu.frameskip) snprintf(b, sizeof b, "VITESSE %3.0f %%   1 IMAGE SUR %d", emu_speed, emu.frameskip + 1);
        else snprintf(b, sizeof b, "VITESSE %3.0f %%   TRAMES %llu", emu_speed, (unsigned long long)emu.frames);
        gfx_text(34, py + 6, b, 2, emu_speed >= 90 ? C_OK : emu_speed >= 30 ? C_WARN : C_BAD);
        {
            py += 40;
            if (!emu.fb_valid) { gfx_text(34, py, "AUCUNE IMAGE VI POUR L'INSTANT", 2, C_TEXT); py += 30; }
            snprintf(b, sizeof b, "MICROCODE     %s", emu.ucode >= 0 ? gsp_ucode_name(emu.ucode) : "-"); gfx_text(34, py, b, 2, emu.ucode >= 0 ? C_OK : C_DIM); py += 24;
            snprintf(b, sizeof b, "TRIANGLES     %u (ELIM. %u)", (unsigned)emu.gsp.tris_in, (unsigned)emu.gsp.tris_culled); gfx_text(34, py, b, 2, emu.gsp.tris_in ? C_OK : C_DIM); py += 24;
            snprintf(b, sizeof b, "CMD INCONNUES %u", (unsigned)emu.gsp.unknown); gfx_text(34, py, b, 2, emu.gsp.unknown ? C_WARN : C_TEXT); py += 24;
            snprintf(b, sizeof b, "INSTRUCTIONS  %llu M", (unsigned long long)(emu.cpu.instructions / 1000000)); gfx_text(34, py, b, 2, C_TEXT); py += 24;
            snprintf(b, sizeof b, "PC            %08X", (unsigned)emu.cpu.pc); gfx_text(34, py, b, 2, C_TEXT); py += 24;
            snprintf(b, sizeof b, "TACHES GFX    %u", (unsigned)emu.gfx_tasks); gfx_text(34, py, b, 2, emu.gfx_tasks ? C_OK : C_DIM); py += 24;
            snprintf(b, sizeof b, "TACHES SON    %u (%s)", (unsigned)emu.audio_tasks, emu.audio_abi >= 0 ? aspd_abi_name(emu.audio_abi) : "-");
            gfx_text(34, py, b, 2, emu.audio_tasks ? (emu.audio_abi > 0 ? C_OK : C_WARN) : C_DIM); py += 24;
            snprintf(b, sizeof b, "INTERRUPT. VI %u", (unsigned)emu.vi_intrs); gfx_text(34, py, b, 2, C_TEXT); py += 24;
            snprintf(b, sizeof b, "DMA PI / SI   %u / %u", (unsigned)emu.pi_dmas, (unsigned)emu.si_dmas); gfx_text(34, py, b, 2, C_TEXT); py += 24;
            snprintf(b, sizeof b, "EXCEPTIONS    %llu", (unsigned long long)emu.cpu.exceptions); gfx_text(34, py, b, 2, C_TEXT); py += 24;
            snprintf(b, sizeof b, "ATTENTE SAUTEE %llu %%", (unsigned long long)(emu.cpu.cycles ? emu.cpu.idle_cycles * 100 / emu.cpu.cycles : 0));
            gfx_text(34, py, b, 2, C_TEXT); py += 24;
            if (jit_status == 1) snprintf(b, sizeof b, "RECOMPILATEUR ACTIF (%s) %llu%%", jit_msg,
                                          (unsigned long long)(emu.cpu.instructions ? emu.cpu.jit_instr * 100 / emu.cpu.instructions : 0));
            else if (jit_status < 0) snprintf(b, sizeof b, "INTERPRETEUR : %s", jit_msg);
            else snprintf(b, sizeof b, "INTERPRETEUR (RECOMPILATEUR COUPE)");
            gfx_text(34, py, b, 2, jit_status == 1 ? C_OK : C_WARN); py += 24;
            if (emu.gfx_worker) snprintf(b, sizeof b, "TEMPS CPU %d%% SON %d%% ECRAN %d%%  3D (2E FIL) %d%%", pct_cpu, pct_snd, pct_ui, pct_gfx);
            else snprintf(b, sizeof b, "TEMPS CPU %d%% 3D %d%% SON %d%% ECRAN %d%%", pct_cpu, pct_gfx, pct_snd, pct_ui);
            gfx_text(34, py, b, 2, C_WARN); py += 24;
            {
                int cp, wt;
                plat_perf(&cp, &wt);
                snprintf(b, sizeof b, "COPIE %d.%d MS  ATTENTE %d.%d MS  DESSIN %d.%d MS", cp / 1000, cp / 100 % 10, wt / 1000, wt / 100 % 10,
                         draw_us / 1000, draw_us / 100 % 10);
                gfx_text(34, py, b, 2, C_WARN);
            }
        }
    }
}

#define RUN_N 9
static const char *run_items[RUN_N] = { "REPRENDRE", "RESET", "SAUVER L'ETAT", "CHARGER L'ETAT", "EMPLACEMENT",
                                        "SON", "INFOS A L'ECRAN", "MANETTE", "BIBLIOTHEQUE" };

static void draw_runmenu(void)
{
    int i;
    char b[64];
    draw_run();
    gfx_rect_blend(0, 0, REF_W, REF_H, RGB(0, 0, 0), 170);
    gfx_rect(420, 90, 440, 560, C_PANEL);
    gfx_frame(420, 90, 440, 560, 2, C_ACCENT);
    gfx_text(420 + (440 - gfx_text_width("MENU", 4)) / 2, 108, "MENU", 4, C_TEXT);
    for (i = 0; i < RUN_N; i++) {
        float y = 168 + i * 52;
        int on = i == run_sel;
        if (i == 4) snprintf(b, sizeof b, "< %s : %d >", run_items[i], emu_slot);
        else if (i == 5) snprintf(b, sizeof b, "%s : %s", run_items[i], emu_sound ? "OUI" : "NON");
        else if (i == 6) snprintf(b, sizeof b, "%s : %s", run_items[i], emu_info ? "OUI" : "NON");
        else snprintf(b, sizeof b, "%s", run_items[i]);
        gfx_rect(450, y, 380, 42, on ? C_ACCENT : RGB(40, 42, 56));
        gfx_text(450 + (380 - gfx_text_width(b, 2)) / 2, y + 14, b, 2, C_TEXT);
    }
}

static void update_runmenu(const Pad *m)
{
    int dx, dy;
    char path[400], msg[64];
    nav(m, &dx, &dy);
    if (dy) run_sel = (run_sel + dy + RUN_N) % RUN_N;
    if (run_sel == 4 && dx) emu_slot = (emu_slot + dx + 10) % 10;
    if (m->pressed & (B_CIRCLE | B_SELECT)) screen = SCR_RUN;
    if (m->pressed & B_CROSS) {
        switch (run_sel) {
        case 0: screen = SCR_RUN; break;
        case 1: {
            uint8_t *rom = emu.rom; uint32_t sz = emu.rom_size; int cic = emu.cic, pal = emu.pal, ram = emu.rdram_size > (4u << 20);
            cart_saves_flush();
            n64_free(&emu);
            n64_init(&emu, rom, sz, cic, pal, ram, emu_save_type);
            emu_setup();
            screen = SCR_RUN;
            break;
        }
        case 2:
            cart_saves_flush();
            state_path(path, sizeof path);
            snprintf(msg, sizeof msg, n64_state_save(&emu, path) == 0 ? "ETAT %d SAUVE" : "ECHEC DE LA SAUVEGARDE (ETAT %d)", emu_slot);
            show_toast(msg);
            screen = SCR_RUN;
            break;
        case 3: {
            int r;
            state_path(path, sizeof path);
            r = n64_state_load(&emu, path);
            if (r == 0) { snprintf(msg, sizeof msg, "ETAT %d CHARGE", emu_slot); screen = SCR_RUN; }
            else if (r == -1) snprintf(msg, sizeof msg, "AUCUN ETAT DANS L'EMPLACEMENT %d", emu_slot);
            else snprintf(msg, sizeof msg, "ETAT %d INCOMPATIBLE (AUTRE VERSION/ROM)", emu_slot);
            show_toast(msg);
            break;
        }
        case 4: emu_slot = (emu_slot + 1) % 10; break;
        case 5: emu_sound ^= 1; break;
        case 6: emu_info ^= 1; break;
        case 7: pad_from = SCR_RUNMENU; pad_sel = 0; screen = SCR_PAD; break;
        case 8: emu_stop(); screen = SCR_LIBRARY; break;
        }
    }
}

/* ---------------------------------------------------------------- manette */

#define PAD_N 5

static void pad_button(float x, float y, float r, uint32_t col, int on, const char *lbl)
{
    disc(x, y, r, on ? RGB(255, 255, 255) : col);
    if (lbl) gfx_text(x - gfx_text_width(lbl, 1.5f) / 2, y - 5, lbl, 1.5f, on ? RGB(20, 20, 20) : RGB(255, 255, 255));
}

static void draw_controller(float ox, float oy, const N64Pad *p)
{
    uint32_t body = RGB(120, 122, 132), dark = RGB(70, 72, 82);
    uint16_t b = p->buttons;
    /* corps et trois poignees (dessin original simplifie) */
    gfx_rect(ox + 40, oy + 40, 440, 120, body);
    disc(ox + 70, oy + 100, 62, body);
    disc(ox + 450, oy + 100, 62, body);
    gfx_rect(ox + 20, oy + 100, 100, 160, body);
    gfx_rect(ox + 210, oy + 120, 100, 170, body);
    gfx_rect(ox + 400, oy + 100, 100, 160, body);
    /* epaules */
    gfx_rect(ox + 40, oy + 26, 110, 16, (b & N64_L) ? RGB(255, 255, 255) : dark);
    gfx_rect(ox + 370, oy + 26, 110, 16, (b & N64_R) ? RGB(255, 255, 255) : dark);
    gfx_text(ox + 88, oy + 6, "L", 2, C_TEXT);
    gfx_text(ox + 420, oy + 6, "R", 2, C_TEXT);
    /* croix */
    gfx_rect(ox + 50, oy + 92, 60, 18, dark);
    gfx_rect(ox + 71, oy + 71, 18, 60, dark);
    if (b & N64_DU) gfx_rect(ox + 71, oy + 71, 18, 20, RGB(255, 255, 255));
    if (b & N64_DD) gfx_rect(ox + 71, oy + 111, 18, 20, RGB(255, 255, 255));
    if (b & N64_DL) gfx_rect(ox + 50, oy + 92, 20, 18, RGB(255, 255, 255));
    if (b & N64_DR) gfx_rect(ox + 90, oy + 92, 20, 18, RGB(255, 255, 255));
    /* start */
    pad_button(ox + 260, oy + 80, 14, RGB(210, 50, 50), b & N64_START, "S");
    /* stick */
    disc(ox + 260, oy + 150, 30, dark);
    disc(ox + 260 + p->x * 0.25f, oy + 150 - p->y * 0.25f, 18, RGB(170, 172, 182));
    /* Z (gachette sous la poignee centrale) */
    gfx_rect(ox + 238, oy + 230, 44, 26, (b & N64_Z) ? RGB(255, 255, 255) : dark);
    gfx_text(ox + 254, oy + 237, "Z", 2, (b & N64_Z) ? RGB(20, 20, 20) : C_TEXT);
    /* A, B */
    pad_button(ox + 410, oy + 130, 17, RGB(40, 90, 200), b & N64_A, "A");
    pad_button(ox + 380, oy + 100, 17, RGB(40, 150, 70), b & N64_B, "B");
    /* boutons C */
    pad_button(ox + 450, oy + 60, 12, RGB(230, 190, 30), b & N64_CU, NULL);
    pad_button(ox + 450, oy + 104, 12, RGB(230, 190, 30), b & N64_CD, NULL);
    pad_button(ox + 428, oy + 82, 12, RGB(230, 190, 30), b & N64_CL, NULL);
    pad_button(ox + 472, oy + 82, 12, RGB(230, 190, 30), b & N64_CR, NULL);
}

static void draw_pad(const Pad pads[2])
{
    static const char *map[] = {
        "STICK GAUCHE      STICK ANALOGIQUE",
        "CROIX DIR.        CROIX N64",
        BTN_X " / " BTN_SQ "             A / B",
        "STICK DROIT       BOUTONS C",
        "L1 / R1           L / R",
        "L2 (OU R2)        Z",
        "START             START",
        "SELECT            MENU DE L'EMULATEUR",
        NULL
    };
    N64Pad p;
    char b[96];
    int i;
    float cx = 1000, cy = 500, R = 100;
    n64pad_convert(&pads[0], &cfg.pad, &p);

    gfx_clear(C_BG);
    gfx_text(60, 36, "MANETTE", 5, C_TEXT);
    gfx_text(60 + gfx_text_width("MANETTE ", 5), 54, "TEST EN DIRECT (MANETTE 1)", 2, C_DIM);
    draw_controller(80, 110, &p);

    for (i = 0; map[i]; i++) gfx_text(640, 110 + i * 27, map[i], 2, C_TEXT);

    /* zone du stick : amplitude N64 et zone morte */
    gfx_rect(cx - R - 20, cy - R - 20, 2 * R + 40, 2 * R + 40, C_PANEL);
    ring(cx, cy, R, 2, C_LINE);
    ring(cx, cy, R * cfg.pad.deadzone / 100.0f, 2, C_WARN);
    gfx_line(cx - R, cy, cx + R, cy, 1, C_LINE);
    gfx_line(cx, cy - R, cx, cy + R, 1, C_LINE);
    disc(cx + pads[0].lx * R, cy + pads[0].ly * R, 5, C_DIM);                     /* brut PS3 */
    disc(cx + p.x * R / cfg.pad.range, cy - p.y * R / cfg.pad.range, 8, C_ACCENT);  /* N64 */
    snprintf(b, sizeof b, "N64  X %+4d  Y %+4d", p.x, p.y);
    gfx_text(cx - R - 10, cy + R + 30, b, 2, C_ACCENT);
    snprintf(b, sizeof b, "BOUTONS %04X", p.buttons);
    gfx_text(cx - R - 10, cy + R + 56, b, 2, C_DIM);

    /* reglages */
    {
        static const char *names[PAD_N] = { "ZONE MORTE", "AMPLITUDE DU STICK", "BOUTON Z", "C-HAUT / C-DROITE SUR " BTN_TR " / " BTN_O, "RETOUR" };
        for (i = 0; i < PAD_N; i++) {
            float y = 430 + i * 44;
            int on = i == pad_sel;
            gfx_rect(80, y, 640, 38, on ? C_SEL : C_PANEL);
            if (on) gfx_rect(80, y, 5, 38, C_ACCENT);
            gfx_text(100, y + 12, names[i], 2, C_TEXT);
            b[0] = 0;
            if (i == 0) snprintf(b, sizeof b, "%d %%", cfg.pad.deadzone);
            if (i == 1) snprintf(b, sizeof b, "%d", cfg.pad.range);
            if (i == 2) snprintf(b, sizeof b, "%s", cfg.pad.z_on_r2 ? "R2" : "L2");
            if (i == 3) snprintf(b, sizeof b, "%s", cfg.pad.c_face ? "OUI" : "NON");
            if (b[0]) gfx_text(700 - gfx_text_width(b, 2), y + 12, b, 2, on ? C_ACCENT : C_DIM);
        }
    }
    footer("SELECT + HAUT/BAS : REGLAGE   SELECT + GAUCHE/DROITE : MODIFIER   SELECT + " BTN_O " : RETOUR");
}

static void update_pad(const Pad *m)
{
    /* toutes les touches servent au test : les reglages passent par SELECT maintenu */
    if (m->held & B_SELECT) {
        int dx, dy;
        nav(m, &dx, &dy);
        if (dy) pad_sel = (pad_sel + dy + PAD_N) % PAD_N;
        if (dx) {
            if (pad_sel == 0) cfg.pad.deadzone = clampi(cfg.pad.deadzone + dx * 2, 0, 30);
            if (pad_sel == 1) cfg.pad.range = clampi(cfg.pad.range + dx, 60, 85);
            if (pad_sel == 2) cfg.pad.z_on_r2 ^= 1;
            if (pad_sel == 3) cfg.pad.c_face ^= 1;
        }
        if ((m->pressed & B_CROSS) && pad_sel == PAD_N - 1) { config_save(); screen = pad_from; }
        if (m->pressed & B_CIRCLE) { config_save(); screen = pad_from; }
    }
}

/* ---------------------------------------------------------------- options */

#define OPT_N 17
#define OPT_EDIT 11
static const char *opt_names[OPT_N] = {
    "FORMAT D'IMAGE", "TAILLE DE L'IMAGE", "REGION / FREQUENCE", "EXPANSION PAK (8 MO)",
    "SAUT D'IMAGES", "CYCLES PAR INSTRUCTION", "SAUT DES BOUCLES D'ATTENTE", "QUALITE 3D", "AFFICHAGE DU JEU",
    "RECOMPILATEUR (WEBMAN / PS3MAPI)", "DESSIN 3D EN PARALLELE (2E FIL)", "MANETTE : TEST ET REGLAGES", "RECHARGER LES ROMS", "AIDE : ROMS", "A PROPOS DES ETAPES", "RETOUR", "QUITTER L'EMULATEUR"
};
static const char *opt_help[OPT_N] = {
    "4:3 COMME SUR UN TELEVISEUR DE L'EPOQUE, OU 16:9 ETIRE. (UTILISE DES L'ETAPE 2)",
    "AJUSTE L'IMAGE SI LE TELEVISEUR ROGNE LES BORDS (OVERSCAN).",
    "AUTO : D'APRES LA LETTRE DE REGION DE LA ROM (E = NTSC, P = PAL...).",
    "AUTO : ACTIVE SI LE JEU EN A BESOIN (MAJORA'S MASK, DONKEY KONG 64).",
    "AUTO : NE DESSINE QU'UNE IMAGE SUR 2 A 4 SI LE JEU RALENTIT (LE JEU GARDE SA VITESSE).",
    "2 = RAPIDE (CONSEILLE). 1 = EXACT MAIS LENT. 3 = PLUS RAPIDE, PEUT CASSER DES JEUX.",
    "SAUTE LE TEMPS OU LE JEU NE FAIT QU'ATTENDRE (GROS GAIN). A COUPER EN CAS DE BUG.",
    "RAPIDE : TEXTURES NON LISSEES (4X MOINS DE LECTURES). TRES RAPIDE : SANS PERSPECTIVE.",
    "RSX : AGRANDI ET LISSE PAR LA CARTE GRAPHIQUE (RAPIDE). LOGICIEL : SI L'IMAGE EST FAUSSE.",
    "TRADUIT LE CODE N64 EN CODE PS3 (BIEN PLUS RAPIDE). DEMANDE PS3MAPI (COBRA / WEBMAN MOD).",
    "LA 3D EST DESSINEE PAR LE 2E FIL DU PROCESSEUR PS3, EN MEME TEMPS QUE LE JEU TOURNE.",
    "VISUALISE LA MANETTE N64 VIRTUELLE ET REGLE LE STICK.",
    "RELIT LES DOSSIERS DE ROMS (CLE USB COMPRISE).",
    "", "", "", "RETOUR AU XMB."
};

static void opt_value(int i, char *out, int n)
{
    static const char *asp[] = { "4:3", "16:9" };
    static const char *reg[] = { "AUTO", "NTSC 60 HZ", "PAL 50 HZ" };
    static const char *exp[] = { "AUTO", "TOUJOURS", "JAMAIS" };
    switch (i) {
    case 0: snprintf(out, n, "%s", asp[cfg.aspect]); break;
    case 1: snprintf(out, n, "%d %%", cfg.size); break;
    case 2: snprintf(out, n, "%s", reg[cfg.region]); break;
    case 3: snprintf(out, n, "%s", exp[cfg.expansion]); break;
    case 4: if (cfg.frameskip < 0) snprintf(out, n, "AUTO"); else if (!cfg.frameskip) snprintf(out, n, "AUCUN"); else snprintf(out, n, "1 IMAGE SUR %d", cfg.frameskip + 1); break;
    case 5: snprintf(out, n, "%d", cfg.cpi); break;
    case 6: snprintf(out, n, "%s", cfg.idle ? "OUI" : "NON"); break;
    case 7: snprintf(out, n, "%s", cfg.gfxq == 0 ? "NORMALE" : cfg.gfxq == 1 ? "RAPIDE" : "TRES RAPIDE"); break;
    case 8: snprintf(out, n, "%s", plat_rsx_blit() ? (cfg.display ? "LOGICIEL" : "RSX") : "LOGICIEL"); break;
    case 10: snprintf(out, n, "%s", cfg.par ? (emu_on && !emu.gfx_worker ? "OUI (INDISPONIBLE)" : "OUI") : "NON"); break;
    case 9: snprintf(out, n, "%s", !cfg.jit ? "NON" : jit_status == 1 ? "OUI (ACTIF)" : jit_status < 0 ? "OUI (INDISPONIBLE)" : "OUI"); break;
    default: out[0] = 0;
    }
}

static void opt_change(int i, int d)
{
    switch (i) {
    case 0: cfg.aspect ^= 1; break;
    case 1: cfg.size = clampi(cfg.size + d * 2, 80, 100); break;
    case 2: cfg.region = (cfg.region + d + 3) % 3; break;
    case 3: cfg.expansion = (cfg.expansion + d + 3) % 3; break;
    case 4: cfg.frameskip = (cfg.frameskip + 1 + d + 5) % 5 - 1; break;
    case 5: cfg.cpi = (cfg.cpi - 1 + d + 3) % 3 + 1; break;
    case 6: cfg.idle ^= 1; break;
    case 7: cfg.gfxq = (cfg.gfxq + d + 3) % 3; break;
    case 8: cfg.display ^= 1; break;
    case 9: cfg.jit ^= 1; jit_apply(); break;
    case 10: cfg.par ^= 1; jit_apply(); break;
    }
    if (emu_on) { emu.cpu.cpi = (unsigned)cfg.cpi; emu.cpu.idle_skip = cfg.idle; emu.rdp.fast = cfg.gfxq; if (cfg.frameskip >= 0) emu.frameskip = cfg.frameskip; }
}

static int help_page;

static void draw_options(void)
{
    int i;
    char v[64];
    gfx_clear(C_BG);
    gfx_text(60, 36, "OPTIONS", 5, C_TEXT);
    for (i = 0; i < OPT_N; i++) {
        float y = 90 + i * 30;
        int on = i == opt_sel;
        gfx_rect(60, y, 1160, 27, on ? C_SEL : C_PANEL);
        if (on) gfx_rect(60, y, 5, 27, C_ACCENT);
        gfx_text(84, y + 6, opt_names[i], 2, on ? RGB(255, 255, 255) : C_TEXT);
        opt_value(i, v, sizeof v);
        if (v[0]) {
            char b[80];
            snprintf(b, sizeof b, on ? "< %s >" : "%s", v);
            gfx_text(1196 - gfx_text_width(b, 2), y + 6, b, 2, on ? C_ACCENT : C_DIM);
        }
    }
    gfx_rect(60, 618, 1160, 2, C_LINE);
    gfx_text(60, 636, opt_help[opt_sel], 2, C_DIM);
    footer(BTN_X " CHOISIR   GAUCHE/DROITE MODIFIER   " BTN_O " RETOUR");
}

static int update_options(const Pad *m)
{
    int dx, dy;
    nav(m, &dx, &dy);
    if (dy) opt_sel = (opt_sel + dy + OPT_N) % OPT_N;
    if (dx && opt_sel < OPT_EDIT) opt_change(opt_sel, dx);
    if (m->pressed & B_CROSS) {
        if (opt_sel < OPT_EDIT) opt_change(opt_sel, 1);
        else if (opt_sel == 11) { pad_from = SCR_OPTIONS; pad_sel = 0; screen = SCR_PAD; }
        else if (opt_sel == 12) {
            char b[64];
            rom_scan(&lib);
            config_load();
            refilter();
            snprintf(b, sizeof b, "%d ROM(S) TROUVEE(S)", lib.count);
            show_toast(b);
        } else if (opt_sel == 13) { help_page = 1; help_from = SCR_OPTIONS; screen = SCR_HELP; }
        else if (opt_sel == 14) { help_page = 2; help_from = SCR_OPTIONS; screen = SCR_HELP; }
        else if (opt_sel == 15) { config_save(); screen = options_from; }
        else { config_save(); return 0; }
    }
    if (m->pressed & (B_CIRCLE | B_START)) { config_save(); screen = options_from; }
    return 1;
}

/* ---------------------------------------------------------------- aide */

static void draw_help(void)
{
    static const char *roms[] = {
        "AUCUN BIOS N'EST NECESSAIRE.",
        "",
        "FORMATS : .Z64 (RECOMMANDE), .V64, .N64 - CONVERTIS AUTOMATIQUEMENT.",
        "LE TITRE, LE CODE ET LA REGION SONT LUS DANS L'EN-TETE DE LA ROM.",
        "",
        "DOSSIERS LUS :",
        "  /DEV_USB000/N64/   (ET USB001 A USB006)",
        "  /DEV_HDD0/N64/   ET   /DEV_HDD0/ROMS/N64/",
        "  /DEV_HDD0/GAME/NSIX00001/USRDIR/ROMS/",
        NULL
    };
    static const char *steps[] = {
        "ETAPE 1 (FAITE) : INTERFACE, LECTURE ET VERIFICATION DES ROMS,",
        "                          MANETTE N64 VIRTUELLE.",
        "ETAPE 2 (FAITE) : PROCESSEUR VR4300, MEMOIRE, VIDEO VI, MANETTES.",
        "ETAPE 3 (FAITE) : GRAPHISMES 3D F3D/F3DEX/F3DEX2 (RDP LOGICIEL).",
        "ETAPE 4 (FAITE) : SON, SAUVEGARDES EEPROM/SRAM, ETATS (10 EMPL.).",
        "ETAPE 5 (FAITE) : VITESSE : ATTENTES SAUTEES, SAUT D'IMAGES AUTO, RSX.",
        "ETAPE 6 (CETTE VERSION) : RECOMPILATEUR MIPS -> POWERPC (PS3MAPI),",
        "                          DESSIN 3D SUR LE 2E FIL DU PROCESSEUR.",
        "",
        "JEUX PRIORITAIRES : SUPER MARIO 64, MARIO KART 64, OCARINA OF TIME.",
        NULL
    };
    const char **t = help_page == 1 ? roms : steps;
    int i;
    gfx_clear(C_BG);
    gfx_text(60, 40, help_page == 1 ? "AIDE : ROMS" : "LES ETAPES DU PROJET", 5, C_TEXT);
    for (i = 0; t[i]; i++) gfx_text(80, 130 + i * 38, t[i], 2, C_TEXT);
    footer(BTN_O " RETOUR");
}

/* ---------------------------------------------------------------- titre */

static void draw_splash(void)
{
    float t = splash_t / 60.0f;
    gfx_clear(RGB(0, 0, 0));
    cube(REF_W / 2.0f, 205, 115, t, 5);
    gfx_text((REF_W - gfx_text_width("N64", 9)) / 2, 430, "N64", 9, RGB(255, 255, 255));
    gfx_text((REF_W - gfx_text_width("EMULATEUR NINTENDO 64 POUR PS3 - " VERSION, 2)) / 2, 520,
             "EMULATEUR NINTENDO 64 POUR PS3 - " VERSION, 2, C_ACCENT);
    if ((splash_t / 30) & 1)
        gfx_text((REF_W - gfx_text_width("APPUYEZ SUR " BTN_X, 3)) / 2, 620, "APPUYEZ SUR " BTN_X, 3, C_TEXT);
}

/* ---------------------------------------------------------------- boucle */

void ui_init(void)
{
    crc_init();
    rom_scan(&lib);
    config_load();
    refilter();
}

void ui_shutdown(void) { config_save(); boot_free(); emu_stop(); }

int ui_frame(Surface *s, const Pad pads[2])
{
    Pad m = merge(pads);
    int alive = 1;
    frame_no++;
    gfx_begin(s);

    switch (screen) {
    case SCR_SPLASH:
        splash_t++;
        draw_splash();
        if ((m.pressed & (B_CROSS | B_START)) || splash_t > 300) screen = SCR_LIBRARY;
        break;
    case SCR_LIBRARY:
        update_library(&m);
        if (screen == SCR_LIBRARY) draw_library(0);
        else if (screen == SCR_SEARCH) draw_search();
        else if (screen == SCR_OPTIONS) draw_options();
        else draw_boot();
        break;
    case SCR_SEARCH:
        update_search(&m);
        if (screen == SCR_SEARCH) draw_search(); else draw_library(0);
        break;
    case SCR_OPTIONS:
        alive = update_options(&m);
        if (screen == SCR_HELP) draw_help();
        else if (screen == SCR_PAD) draw_pad(pads);
        else if (screen == SCR_OPTIONS) draw_options();
        else draw_library(0);
        break;
    case SCR_BOOT:
        update_boot(&m);
        if (screen == SCR_BOOT) draw_boot();
        else if (screen == SCR_RUN) draw_run();
        else draw_library(0);
        break;
    case SCR_PAD:
        update_pad(&m);
        if (screen == SCR_PAD) draw_pad(pads);
        else if (screen == SCR_RUNMENU) draw_runmenu();
        else draw_options();
        break;
    case SCR_RUN:
        if (m.pressed & B_SELECT) { screen = SCR_RUNMENU; run_sel = 0; draw_runmenu(); break; }
        emu_step(pads);
        {
            /* on ne redessine l'ecran PS3 que s'il y a du nouveau */
            uint64_t t = plat_time_us();
            int direct = run_direct() && emu.fb_valid;
            if (!emu.fb_new && !toast_t && emu.fb_valid && t - last_draw < 500000) return alive ? 2 : 0;
            if (direct && t - last_draw < 500000 && !toast_t && run_info.px) {
                /* image nouvelle mais interface inchangee : rien a dessiner cote PPU */
                run_info.canvas_dirty = 0;
                return alive ? 3 : 0;
            }
            draw_run();
            if (direct) {
                if (toast_t > 0) {
                    float tw = gfx_text_width(toast, 2) + 40;
                    if (run_info.novl < 2) gfx_rect_to_px((REF_W - tw) / 2, 570, tw, 40, run_info.ovl[run_info.novl++]);
                    draw_toast();
                }
                last_draw = plat_time_us();
                draw_us = (draw_us * 7 + (int)(last_draw - t)) / 8;
                return alive ? 3 : 0;
            }
            last_draw = plat_time_us();
            draw_us = (draw_us * 7 + (int)(last_draw - t)) / 8;
        }
        break;
    case SCR_RUNMENU:
        update_runmenu(&m);
        if (screen == SCR_RUNMENU) draw_runmenu();
        else if (screen == SCR_RUN) draw_run();
        else if (screen == SCR_PAD) draw_pad(pads);
        else draw_library(0);
        break;
    case SCR_HELP:
        draw_help();
        if (m.pressed & (B_CIRCLE | B_CROSS | B_START)) screen = help_from;
        break;
    }
    draw_toast();
    return alive;
}
