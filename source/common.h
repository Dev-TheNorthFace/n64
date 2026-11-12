/* N64 - emulateur Nintendo 64 pour PS3
 * common.h : types partages entre le coeur, l'interface et la plateforme. */
#ifndef N64_COMMON_H
#define N64_COMMON_H

#include <stdint.h>

/* ---- Manette ---- */
enum {
    B_UP = 1 << 0, B_DOWN = 1 << 1, B_LEFT = 1 << 2, B_RIGHT = 1 << 3,
    B_CROSS = 1 << 4, B_CIRCLE = 1 << 5, B_SQUARE = 1 << 6, B_TRIANGLE = 1 << 7,
    B_L1 = 1 << 8, B_R1 = 1 << 9, B_L2 = 1 << 10, B_R2 = 1 << 11,
    B_START = 1 << 12, B_SELECT = 1 << 13, B_L3 = 1 << 14, B_R3 = 1 << 15
};

typedef struct {
    int connected;
    uint32_t held;      /* boutons enfonces */
    uint32_t pressed;   /* boutons enfonces depuis cette image */
    float lx, ly;       /* stick gauche  -1..1 */
    float rx, ry;       /* stick droit   -1..1 */
} Pad;

/* ---- Surface d'affichage (XRGB 32 bits) ---- */
typedef struct {
    uint32_t *px;
    int w, h;
    int pitch;          /* en pixels */
} Surface;

/* ---- Plateforme (implementee par main_ps3.c ou tools/host_main.c) ---- */
typedef void (*DirCallback)(const char *path, void *user);

int  plat_list_dir(const char *dir, DirCallback cb, void *user);
const char *plat_data_dir(void);     /* dossier pour config + cartes */
const char *const *plat_rom_dirs(void);  /* dossiers de ROMs/BIOS, liste terminee par NULL */
/* son : pousse n paires d'echantillons stereo 16 bits (G, D) a 48 kHz */
void plat_audio_push(const int16_t *s, int n);
uint64_t plat_time_us(void);         /* horloge monotone en microsecondes */
/* mesures de l'affichage (copie vers le RSX, attente de l'image) en microsecondes */
void plat_perf(int *copy_us, int *wait_us);
int  plat_rsx_blit(void);             /* 1 = la plateforme sait agrandir l'image avec le RSX */
/* recompilateur : branche le recompilateur sur le processeur (VR4300 *), 0 si OK ;
 * why recoit l'explication (version de PS3MAPI ou raison de l'echec) */
int  plat_jit_attach(void *cpu, const char **why);
/* fil secondaire pour le dessin 3D (N64Worker *), NULL si impossible */
void *plat_gfx_worker(void);

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

#endif
