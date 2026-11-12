#ifndef N64_UI_H
#define N64_UI_H
#include "common.h"

void ui_init(void);
int  ui_frame(Surface *s, const Pad pads[2]);   /* 0 = quitter */
void ui_shutdown(void);

enum { SCR_SPLASH, SCR_LIBRARY, SCR_SEARCH, SCR_OPTIONS, SCR_BOOT, SCR_PAD, SCR_HELP, SCR_RUN, SCR_RUNMENU };
int  ui_screen(void);

/* affichage accelere par le RSX (ecran de jeu) : ui_frame renvoie 3 et decrit
 * l'image du jeu a agrandir et les rectangles d'interface a poser par-dessus */
typedef struct {
    const uint32_t *px; int w, h;          /* image N64 (XRGB) */
    int dst[4];                             /* rectangle ecran de l'image */
    int canvas_dirty;                       /* le canevas a ete redessine */
    int novl; int ovl[2][4];               /* rectangles du canevas a recopier par-dessus */
} UiRun;
const UiRun *ui_run_info(void);

#endif
