/* n64.h : machine Nintendo 64 (etape 5).
 *  - RDRAM 4/8 Mo, ROM de cartouche, memoire du RSP (DMEM/IMEM)
 *  - MI (interruptions), VI (video), AI (son), PI (DMA cartouche),
 *    SI + PIF (manettes, EEPROM), SP (taches RSP en haut niveau)
 *  - demarrage HLE : simule IPL1/IPL2/IPL3 (aucun BIOS necessaire) */
#ifndef N64_N64_H
#define N64_N64_H

#include "vr4300.h"
#include "rdp.h"
#include "gsp.h"
#include "aspd.h"

#define N64_FB_MAXW 640
#define N64_FB_MAXH 480

/* fil d'execution secondaire (fourni par la plateforme) : les taches graphiques y sont
 * dessinees pendant que le processeur N64 continue sur le fil principal, comme le vrai
 * RSP/RDP travaille en parallele du CPU. start() lance fn(arg) ; wait() attend la fin ;
 * done passe a 1 quand fn est terminee. */
typedef struct N64Worker {
    void (*start)(struct N64Worker *w, void (*fn)(void *), void *arg);
    void (*wait)(struct N64Worker *w);
    volatile int done;
} N64Worker;

enum { MI_SP = 1, MI_SI = 2, MI_AI = 4, MI_VI = 8, MI_PI = 16, MI_DP = 32 };

typedef struct {
    VR4300 cpu;
    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *rom;
    uint32_t rom_size;
    int cic, pal;

    uint8_t sp_mem[0x2000];
    uint32_t sp_reg[8], sp_pc;
    uint32_t dp_reg[8];
    uint32_t mi_mode, mi_intr, mi_mask;
    uint32_t vi[14];
    uint32_t ai[6];
    uint64_t ai_end_cycle;
    int ai_busy;                /* nombre de tampons dans la file AI (0..2) */
    uint32_t ai_q_addr[2], ai_q_len[2];
    uint32_t ai_freq;
    uint32_t ai_phase;          /* reechantillonnage vers 48 kHz (Q16) */
    int16_t ai_prev[2];
    uint32_t pi[13];
    uint32_t ri[8];
    uint32_t si[7];
    uint8_t pif_ram[64];
    uint8_t eeprom[2048];
    int eeprom_type;            /* 0 aucune, 1 = 4 kbit, 2 = 16 kbit */
    int eeprom_dirty;
    uint8_t sram[0x8000];       /* SRAM 256 kbit (Zelda OoT...) */
    int sram_enabled, sram_dirty;

    /* manettes */
    uint16_t pad_buttons[4];
    int8_t pad_x[4], pad_y[4];
    int pad_connected[4];

    /* temps */
    uint64_t cycles_per_frame;
    int line;                   /* demi-ligne courante (pour reprendre une trame) */
    uint64_t frames;

    /* statistiques (affichees a l'ecran) */
    uint32_t gfx_tasks, audio_tasks, other_tasks, vi_intrs, si_dmas, pi_dmas, dp_lists;
    uint32_t last_dlist;
    int ucode;                  /* microcode graphique reconnu (-1 = inconnu) */
    uint32_t ucode_addr;

    /* graphismes (etape 3) */
    Rdp rdp;
    Gsp gsp;
    int gfx_enabled;            /* 0 = ignorer les taches graphiques (diagnostic) */
    int frameskip;              /* etape 5 : 0 = tout dessiner, n = dessiner 1 image sur n+1 */
    int fs_skipped;             /* la trame courante n'a pas ete dessinee */
    uint32_t frames_skipped;
    int fb_new;                 /* une nouvelle image a ete produite */
    /* profilage (temps en microsecondes) */
    uint64_t (*clock_us)(void);
    uint64_t t_gfx, t_audio;
    /* graphismes en parallele (etape 6) */
    N64Worker *gfx_worker;      /* NULL = taches graphiques dessinees sur place */
    int gfx_pending;            /* une tache graphique est en cours sur l'autre fil */
    int gfx_yielded;            /* le jeu l'a suspendue (pour le son) : elle continue en fond */
    int gfx_waited;             /* l'autre fil a fini cette tache (resultat recupere) */
    uint32_t gfx_job_dlist;
    uint64_t gfx_job_us;

    /* son (etape 4) */
    Aspd aspd;
    int audio_abi;              /* -1 = pas encore reconnu */
    uint32_t audio_ucode_addr;
    char game_id[3];
    uint32_t ai_samples_out;
    void (*audio_out)(void *ctx, const int16_t *stereo, int frames);   /* 48 kHz stereo */
    void *audio_ctx;

    /* image */
    uint32_t fb[N64_FB_MAXW * N64_FB_MAXH];
    int fb_w, fb_h, fb_valid;
} N64;

/* charge une ROM deja en big-endian (la machine garde le pointeur) */
int  n64_init(N64 *m, uint8_t *rom, uint32_t size, int cic, int pal, int ram8mb, int save_type);
void n64_free(N64 *m);
void n64_boot_hle(N64 *m);
/* attend la fin de la tache graphique en cours (autre fil) et signale sa fin au jeu */
void n64_gfx_sync(N64 *m);
/* choisit le fil de dessin (NULL = sur place) ; sans risque meme pendant une tache */
void n64_set_gfx_worker(N64 *m, N64Worker *w);
/* execute au plus 'max_halflines' demi-lignes ; renvoie 1 si une trame est terminee */
int  n64_run(N64 *m, int max_halflines);
void n64_render(N64 *m);
/* etats de sauvegarde : 0 si OK */
int  n64_state_save(N64 *m, const char *path);
int  n64_state_load(N64 *m, const char *path);

#endif
