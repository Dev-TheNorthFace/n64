/* main_ps3.c : couche PlayStation 3 (PSL1GHT) pour l'emulateur N64.
 * - affichage : double tampon RSX a la resolution du XMB, composition en RAM
 * - son : port audio 2 canaux (stereo), 8 blocs de 256 echantillons a 48 kHz
 * - manettes : les deux premieres branchees = controleurs droit et gauche */
#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <io/pad.h>
#include <audio/audio.h>
#include <lv2/sysfs.h>   /* sysFSDirent */
#include <sys/file.h>    /* appels systeme sysLv2Fs* (aucune bibliotheque a lier) */
#include <sys/systime.h> /* sysGetCurrentTime (appel systeme) */
#include <sys/process.h> /* sysProcessGetPid (appel systeme) */
#include <sys/thread.h>  /* sysThreadCreate (2e fil du processeur PPU) */

#include "common.h"
#include "ui.h"
#include "jit.h"
#include "n64.h"

#define CB_SIZE   0x100000
#define HOST_SIZE (32 * 1024 * 1024)
#define GCM_LABEL_INDEX 255
#define APP_DIR "/dev_hdd0/game/NSIX00001/USRDIR"

static gcmContextData *context;
static videoResolution res;
static u32 *fb[2];
static u32 fb_off[2];
static int cur_fb;
static u32 label = 1;
static volatile int running = 1;

/* ------------------------------------------------------------ recompilateur (PS3MAPI)
 * Un programme PS3 ne peut pas ecrire dans sa propre zone de code. PS3MAPI (inclus dans
 * Cobra, utilise par webMAN MOD) offre un appel systeme qui ecrit dans la memoire d'un
 * processus, zone de code comprise : on reserve donc 8 Mo de "nop" dans .text et le
 * recompilateur y ecrit le code PowerPC traduit. Sans PS3MAPI : interpreteur. */

#define JIT_AREA_SIZE (8u << 20)
__asm__(".section .text\n"
        ".balign 4096\n"
        ".globl n64_jit_area\n"
        "n64_jit_area:\n"
        ".rept 2097152\n"          /* 8 Mo = 2 097 152 instructions */
        "nop\n"
        ".endr\n"
        "blr\n"
        ".previous\n");
extern uint32_t n64_jit_area[];

#define PS3MAPI_SYSCALL      8
#define PS3MAPI_OPCODE       0x7777
#define PS3MAPI_CORE_VERSION 0x0011
#define PS3MAPI_SET_PROC_MEM 0x0032

static int ps3mapi_core_version(void)
{
    lv2syscall2(PS3MAPI_SYSCALL, PS3MAPI_OPCODE, PS3MAPI_CORE_VERSION);
    return_to_user_prog(int);
}

static int ps3mapi_set_mem(u32 pid, u64 addr, const void *buf, u64 size)
{
    lv2syscall6(PS3MAPI_SYSCALL, PS3MAPI_OPCODE, PS3MAPI_SET_PROC_MEM, (u64)pid, addr, (u64)(uintptr_t)buf, size);
    return_to_user_prog(int);
}

static u32 jit_pid;
static u64 jit_toc;
static int jit_state;            /* 0 non essaye, 1 disponible, -1 indisponible */
static char jit_why[48];

static void icache_sync(const void *p, u32 n)
{
    uintptr_t a, e = (uintptr_t)p + n;
    for (a = (uintptr_t)p & ~(uintptr_t)127; a < e; a += 128) __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
    for (a = (uintptr_t)p & ~(uintptr_t)127; a < e; a += 128) __asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync\n\tisync" : : : "memory");
}

static int ps3_jit_write(void *ctx, uint32_t off, const uint32_t *w, int n)
{
    (void)ctx;
    if (off + (u32)n * 4 > JIT_AREA_SIZE) return -1;
    if (ps3mapi_set_mem(jit_pid, (u64)(uintptr_t)n64_jit_area + off, w, (u64)n * 4) != 0) return -1;
    icache_sync((const uint8_t *)n64_jit_area + off, (u32)n * 4);
    return 0;
}

/* appel d'un bloc : descripteur de fonction ELF 64 bits { adresse, TOC, environnement } */
typedef uint32_t (*JitFn)(VR4300 *);
static uint32_t ps3_jit_call(void *ctx, uint32_t off, VR4300 *c)
{
    volatile u64 desc[3];
    (void)ctx;
    desc[0] = (u64)(uintptr_t)n64_jit_area + off;
    desc[1] = jit_toc;
    desc[2] = 0;
    return ((JitFn)(uintptr_t)desc)(c);
}

/* adresse reelle d'une fonction C (premier mot de son descripteur) */
static uint64_t ps3_jit_fn_addr(void *ctx, void *fn)
{
    (void)ctx;
    return ((const u64 *)fn)[0];
}

static void jit_probe(void)
{
    static const uint32_t test[2] = { 0x38601234u /* li r3,0x1234 */, 0x4E800020u /* blr */ };
    int v;
    if (jit_state) return;
    jit_state = -1;
    v = ps3mapi_core_version();
    if (v < 0x0120) { snprintf(jit_why, sizeof jit_why, "PS3MAPI ABSENT (COBRA/WEBMAN)"); return; }
    jit_pid = (u32)sysProcessGetPid();
    jit_toc = ((const u64 *)(void *)vr4300_exec1)[1];      /* TOC des fonctions d'aide */
    if (ps3_jit_write(NULL, 0, test, 2) != 0) { snprintf(jit_why, sizeof jit_why, "ECRITURE PS3MAPI REFUSEE"); return; }
    if (ps3_jit_call(NULL, 0, NULL) != 0x1234) { snprintf(jit_why, sizeof jit_why, "TEST DU CODE GENERE RATE"); return; }
    jit_state = 1;
    snprintf(jit_why, sizeof jit_why, "PS3MAPI %X.%02X", (unsigned)v >> 8, (unsigned)v & 0xFF);
}

int plat_jit_attach(void *cpu, const char **why)
{
    JitBackend b;
    jit_probe();
    *why = jit_why;
    if (jit_state != 1) return -1;
    memset(&b, 0, sizeof b);
    b.code_size = JIT_AREA_SIZE;
    b.code_base = (uint64_t)(uintptr_t)n64_jit_area;
    b.write = ps3_jit_write;
    b.call = ps3_jit_call;
    b.fn_addr = ps3_jit_fn_addr;
    return jit_init((VR4300 *)cpu, &b);
}

/* ------------------------------------------------------------ 2e fil : dessin 3D
 * Le PPU de la PS3 execute deux fils en meme temps. Le fil principal emule le
 * processeur N64 ; celui-ci dessine les taches graphiques (microcode + RDP).
 * Protocole simple : drapeaux + barrieres memoire (sync), le fil dort par 30 us. */

typedef struct {
    N64Worker w;
    void (*fn)(void *);
    void *arg;
    volatile int go;
    int started;
} Ps3Worker;

static Ps3Worker gfx_thr;

static void gfx_thread_main(void *p)
{
    Ps3Worker *k = (Ps3Worker *)p;
    for (;;) {
        while (!k->go) usleep(30);
        __sync_synchronize();
        k->fn(k->arg);
        __sync_synchronize();
        k->go = 0;
        k->w.done = 1;
    }
}

static void gfx_thr_start(N64Worker *w, void (*fn)(void *), void *arg)
{
    Ps3Worker *k = (Ps3Worker *)w;
    k->fn = fn; k->arg = arg;
    w->done = 0;
    __sync_synchronize();
    k->go = 1;
}

static void gfx_thr_wait(N64Worker *w)
{
    while (!w->done) usleep(20);
    __sync_synchronize();
}

void *plat_gfx_worker(void)
{
    static char name[] = "n64_gfx";
    sys_ppu_thread_t id;
    if (!gfx_thr.started) {
        gfx_thr.w.start = gfx_thr_start;
        gfx_thr.w.wait = gfx_thr_wait;
        gfx_thr.w.done = 1;
        if (sysThreadCreate(&id, gfx_thread_main, &gfx_thr, 1000, 1024 * 1024, 0, name) != 0) return NULL;
        gfx_thr.started = 1;
    }
    return &gfx_thr.w;
}

/* ------------------------------------------------------------ fichiers */

static const char *rom_dirs[] = {
    APP_DIR "/roms",
    "/dev_usb000/n64", "/dev_usb001/n64", "/dev_usb002/n64",
    "/dev_usb003/n64", "/dev_usb006/n64",
    "/dev_hdd0/n64", "/dev_hdd0/ROMS/n64", "/dev_hdd0/roms/n64",
    "/dev_usb000/ROMS/n64",
    NULL
};

const char *const *plat_rom_dirs(void) { return rom_dirs; }
const char *plat_data_dir(void) { return APP_DIR; }

int plat_list_dir(const char *dir, DirCallback cb, void *user)
{
    s32 fd;
    sysFSDirent ent;
    u64 nread;
    char path[1400];
    if (sysLv2FsOpenDir(dir, &fd) != 0) return -1;
    while (sysLv2FsReadDir(fd, &ent, &nread) == 0 && nread > 0) {
        if (ent.d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, ent.d_name);
        cb(path, user);
    }
    sysLv2FsCloseDir(fd);
    return 0;
}

uint64_t plat_time_us(void)
{
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    return (uint64_t)sec * 1000000u + (uint64_t)nsec / 1000u;
}

/* ------------------------------------------------------------ son */

#define FIFO_SIZE 16384
static int16_t fifo[FIFO_SIZE];
static int fifo_r, fifo_w;
static int16_t last_l, last_r;
static u32 aport;
static audioPortConfig acfg;
static int audio_ok;
static u64 wblock;

static int fifo_level(void) { return (fifo_w - fifo_r + FIFO_SIZE) % FIFO_SIZE; }

static void audio_pump(void);

/* echantillons stereo entrelaces (G, D) a 48 kHz ; n = nombre de paires */
void plat_audio_push(const int16_t *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (fifo_level() >= FIFO_SIZE - 4) break;   /* toujours par paires */
        fifo[fifo_w] = s[i * 2];
        fifo[(fifo_w + 1) % FIFO_SIZE] = s[i * 2 + 1];
        fifo_w = (fifo_w + 2) % FIFO_SIZE;
    }
    /* limite la latence a ~100 ms (paires conservees) */
    while (fifo_level() > 9600) fifo_r = (fifo_r + 2) % FIFO_SIZE;
    audio_pump();          /* alimente le port audio meme pendant une longue emulation */
}

static void audio_init(void)
{
    audioPortParam p;
    if (audioInit() != 0) return;
    memset(&p, 0, sizeof p);
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks = AUDIO_BLOCK_8;
    p.attrib = 0;
    p.level = 1.0f;
    if (audioPortOpen(&p, &aport) != 0) return;
    if (audioGetPortConfig(aport, &acfg) != 0) return;
    memset((void *)(u64)acfg.audioDataStart, 0, AUDIO_BLOCK_8 * AUDIO_BLOCK_SAMPLES * 2 * sizeof(float));
    audioPortStart(aport);
    wblock = 1;
    audio_ok = 1;
}

static void audio_pump(void)
{
    u64 ri;
    if (!audio_ok) return;
    ri = *(volatile u64 *)(u64)acfg.readIndex;
    while (wblock != ri) {
        float *buf = (float *)(u64)acfg.audioDataStart + wblock * AUDIO_BLOCK_SAMPLES * 2;
        int i;
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            if (fifo_level() >= 2) {
                last_l = fifo[fifo_r]; last_r = fifo[(fifo_r + 1) % FIFO_SIZE];
                fifo_r = (fifo_r + 2) % FIFO_SIZE;
            } else {                                   /* sous-alimentation : fondu */
                last_l = (int16_t)(last_l * 0.98f); last_r = (int16_t)(last_r * 0.98f);
            }
            buf[i * 2] = last_l / 32768.0f;
            buf[i * 2 + 1] = last_r / 32768.0f;
        }
        wblock = (wblock + 1) % AUDIO_BLOCK_8;
    }
}

static void audio_end(void)
{
    if (!audio_ok) return;
    audioPortStop(aport);
    audioPortClose(aport);
    audioQuit();
}

/* ------------------------------------------------------------ RSX */

static void wait_rsx_idle(void)
{
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, label);
    rsxSetWaitLabel(context, GCM_LABEL_INDEX, label);
    label++;
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, label);
    rsxFlushBuffer(context);
    while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != label) usleep(30);
    label++;
}

static void wait_flip(void)
{
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

static void do_flip(int id)
{
    gcmSetFlip(context, id);
    rsxFlushBuffer(context);
    gcmSetWaitFlip(context);
}

static int video_init(void)
{
    videoState state;
    videoConfiguration vc;
    void *host = memalign(1024 * 1024, HOST_SIZE);
    int i;

    if (!host) return -1;
    if (rsxInit(&context, CB_SIZE, HOST_SIZE, host) != 0 || !context) return -1;
    if (videoGetState(0, 0, &state) != 0) return -1;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0) return -1;

    memset(&vc, 0, sizeof vc);
    vc.resolution = state.displayMode.resolution;
    vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch = res.width * sizeof(u32);
    vc.aspect = state.displayMode.aspect;
    wait_rsx_idle();
    if (videoConfigure(0, &vc, NULL, 0) != 0) return -1;
    videoGetState(0, 0, &state);
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (i = 0; i < 2; i++) {
        fb[i] = (u32 *)rsxMemalign(64, res.width * res.height * 4);
        if (!fb[i]) return -1;
        memset(fb[i], 0, res.width * res.height * 4);
        rsxAddressToOffset(fb[i], &fb_off[i]);
        gcmSetDisplayBuffer(i, fb_off[i], res.width * 4, res.width, res.height);
    }
    gcmResetFlipStatus();
    return 0;
}

/* ------------------------------------------------------------ manettes */

static float axis(u16 v)
{
    float f = ((int)v - 128) / 127.0f;
    if (f > 1) f = 1;
    if (f < -1) f = -1;
    if (f > -0.18f && f < 0.18f) return 0;
    return f;
}

static padData last[MAX_PADS];

static void poll_pads(Pad pads[2])
{
    padInfo info;
    int port, slot = 0;
    uint32_t prev[2] = { pads[0].held, pads[1].held };

    memset(pads, 0, sizeof(Pad) * 2);
    ioPadGetInfo(&info);
    for (port = 0; port < MAX_PADS && slot < 2; port++) {
        padData d;
        Pad *p = &pads[slot];
        if (!info.status[port]) continue;
        ioPadGetData(port, &d);
        if (d.len > 0) last[port] = d;
        d = last[port];
        p->connected = 1;
        if (d.BTN_UP) p->held |= B_UP;
        if (d.BTN_DOWN) p->held |= B_DOWN;
        if (d.BTN_LEFT) p->held |= B_LEFT;
        if (d.BTN_RIGHT) p->held |= B_RIGHT;
        if (d.BTN_CROSS) p->held |= B_CROSS;
        if (d.BTN_CIRCLE) p->held |= B_CIRCLE;
        if (d.BTN_SQUARE) p->held |= B_SQUARE;
        if (d.BTN_TRIANGLE) p->held |= B_TRIANGLE;
        if (d.BTN_L1) p->held |= B_L1;
        if (d.BTN_R1) p->held |= B_R1;
        if (d.BTN_L2) p->held |= B_L2;
        if (d.BTN_R2) p->held |= B_R2;
        if (d.BTN_START) p->held |= B_START;
        if (d.BTN_SELECT) p->held |= B_SELECT;
        if (d.BTN_L3) p->held |= B_L3;
        if (d.BTN_R3) p->held |= B_R3;
        p->lx = axis(d.ANA_L_H);
        p->ly = axis(d.ANA_L_V);
        p->rx = axis(d.ANA_R_H);
        p->ry = axis(d.ANA_R_V);
        p->pressed = p->held & ~prev[slot];
        slot++;
    }
}

/* ------------------------------------------------------------ XMB */

static void sysutil_cb(u64 status, u64 param, void *user)
{
    (void)param; (void)user;
    if (status == SYSUTIL_EXIT_GAME) running = 0;
}

/* ------------------------------------------------------------ main */

static int perf_copy, perf_wait;
void plat_perf(int *copy_us, int *wait_us) { *copy_us = perf_copy; *wait_us = perf_wait; }

/* ---- copies et agrandissement par le RSX (le PPU ne touche plus a la memoire video)
 * Le canevas d'interface et l'image du jeu sont en memoire principale, rendue
 * visible au RSX par gcmMapMainMemory ; le RSX les copie / agrandit lui-meme. */
static u32 game_off;
static int canvas_copies, game_mapped;

/* attend que le RSX ait ecrit la valeur v (delai maxi en microsecondes) */
static int wait_label(u32 v, uint64_t max_us)
{
    uint64_t t0 = plat_time_us();
    while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != v) {
        if (plat_time_us() - t0 > max_us) return 0;
        usleep(20);
    }
    return 1;
}
static u32 *game_buf;
static int rsx_ok;
int plat_rsx_blit(void) { return rsx_ok; }

static int rsx_map(void *p, u32 size, u32 *off) { return gcmMapMainMemory(p, size, off) == 0 ? 0 : -1; }

static void rsx_scale_game(int buf, const UiRun *ri)
{
    gcmTransferScale sc;
    gcmTransferSurface sf;
    int w = ri->w, h = ri->h, i;
    if (w <= 0 || h <= 0 || w > 640 || h > 480 || ri->dst[2] <= 0 || ri->dst[3] <= 0) return;
    for (i = 0; i < h; i++) memcpy(game_buf + i * 640, ri->px + i * w, (size_t)w * 4);
    memset(&sc, 0, sizeof sc);
    memset(&sf, 0, sizeof sf);
    sc.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
    sc.format = GCM_TRANSFER_SCALE_FORMAT_X8R8G8B8;
    sc.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
    sc.clipX = (s16)ri->dst[0]; sc.clipY = (s16)ri->dst[1];
    sc.clipW = (u16)ri->dst[2]; sc.clipH = (u16)ri->dst[3];
    sc.outX = (s16)ri->dst[0]; sc.outY = (s16)ri->dst[1];
    sc.outW = (u16)ri->dst[2]; sc.outH = (u16)ri->dst[3];
    sc.ratioX = (s32)((float)w / (float)ri->dst[2] * 1048576.0f);
    sc.ratioY = (s32)((float)h / (float)ri->dst[3] * 1048576.0f);
    sc.inW = (u16)w; sc.inH = (u16)h;
    sc.pitch = 640 * 4;
    sc.origin = GCM_TRANSFER_ORIGIN_CORNER;
    sc.interp = GCM_TRANSFER_INTERPOLATOR_LINEAR;
    sc.offset = game_off;
    sc.inX = 0; sc.inY = 0;
    sf.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    sf.pitch = (u16)(res.width * 4);
    sf.offset = fb_off[buf];
    rsxSetTransferScaleMode(context, GCM_TRANSFER_MAIN_TO_LOCAL, GCM_TRANSFER_SURFACE);
    rsxSetTransferScaleSurface(context, &sc, &sf);
}

int main(int argc, char **argv)
{
    Pad pads[2];
    Surface ram;
    int first = 1;
    (void)argc; (void)argv;

    if (video_init() != 0) return 1;
    ioPadInit(7);
    audio_init();
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
    sysLv2FsMkdir(APP_DIR "/roms", 0777);
    sysLv2FsMkdir(APP_DIR "/states", 0777);
    sysLv2FsMkdir(APP_DIR "/saves", 0777);

    ram.w = res.width;
    ram.h = res.height;
    ram.pitch = res.width;
    {
        /* canevas + image du jeu en memoire principale alignee sur 1 Mo, visible du RSX */
        u32 csize = (res.width * res.height * 4 + 0xFFFFF) & ~0xFFFFFu;
        ram.px = (uint32_t *)memalign(1024 * 1024, csize);
        if (!ram.px) return 1;
        game_buf = (u32 *)memalign(1024 * 1024, 2 * 1024 * 1024);   /* 640x480x4 = 1,2 Mo */
        rsx_ok = game_buf != NULL;                  /* la projection pour le RSX se fait au premier usage */
        (void)csize;
    }

    memset(pads, 0, sizeof pads);
    ui_init();

    while (running) {
        int r;
        uint64_t t0, t1, t2;
        sysUtilCheckCallback();
        poll_pads(pads);
        r = ui_frame(&ram, pads);
        if (!r) break;
        audio_pump();
        if (r == 2) continue;                       /* rien n'a change : pas de copie ni d'attente */
        t0 = plat_time_us();
        if (first) { gcmResetFlipStatus(); first = 0; }
        else wait_flip();
        t1 = plat_time_us();
        if (r == 3 && rsx_ok && !game_mapped) {
            if (rsx_map(game_buf, 2 * 1024 * 1024, &game_off) == 0) game_mapped = 1;
            else rsx_ok = 0;
        }
        if (r == 3 && rsx_ok) {
            /* ecran de jeu : le RSX agrandit l'image ; le canevas (bords, texte) n'est
             * recopie par le PPU que lorsqu'il change, dans les deux tampons */
            const UiRun *ri = ui_run_info();
            int i;
            u32 pitch = res.width * 4;
            if (ri->canvas_dirty) canvas_copies = 2;
            if (canvas_copies > 0) { memcpy(fb[cur_fb], ram.px, pitch * res.height); canvas_copies--; }
            rsx_scale_game(cur_fb, ri);
            label++;
            rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, label);
            rsxFlushBuffer(context);
            if (!wait_label(label, 300000)) {
                rsx_ok = 0;                          /* le RSX ne repond pas : retour a l'affichage logiciel */
            } else {
                /* panneau d'infos et messages par-dessus l'image (petites copies) */
                for (i = 0; i < ri->novl; i++) {
                    const int *q = ri->ovl[i];
                    int y;
                    for (y = q[1]; y < q[1] + q[3]; y++)
                        memcpy((u8 *)fb[cur_fb] + y * pitch + q[0] * 4, (u8 *)ram.px + y * pitch + q[0] * 4, (size_t)q[2] * 4);
                }
            }
            do_flip(cur_fb);
        } else {
            memcpy(fb[cur_fb], ram.px, res.width * res.height * 4);
            canvas_copies = 0;
            do_flip(cur_fb);
        }
        cur_fb ^= 1;
        t2 = plat_time_us();
        perf_wait = (perf_wait * 7 + (int)(t1 - t0)) / 8;
        perf_copy = (perf_copy * 7 + (int)(t2 - t1)) / 8;
    }

    ui_shutdown();
    audio_end();
    gcmSetWaitFlip(context);
    wait_rsx_idle();
    ioPadEnd();
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    free(ram.px);
    return 0;
}
