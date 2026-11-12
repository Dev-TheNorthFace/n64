/* jit.c : recompilateur dynamique MIPS -> PowerPC 64 (voir jit.h). */
#include "jit.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ etat */

#define PAGE_SHIFT 12
#define MAX_BLOCK_INSNS 64
#define TMP_WORDS 8192

typedef struct {
    JitBackend be;
    uint32_t used;                 /* octets utilises dans la zone de code */
    uint32_t **map;                /* par page de 4 Ko : 1024 entrees (0 = pas de bloc) */
    uint32_t npages;
    uint8_t *codepage;
    uint32_t tmp[TMP_WORDS];
    uint64_t h_exec1, h_load, h_store;   /* adresses des aides C */
    uint64_t flushes, compiled, fallbacks, calls;
} Jit;

/* entree de la table : ((offset / 4) + 1) << 1 | boucle_d_attente */
#define ENTRY(off, idle) (((((off) >> 2) + 1u) << 1) | (uint32_t)(idle))
#define ENTRY_OFF(e) ((((e) >> 1) - 1u) << 2)
#define ENTRY_IDLE(e) ((e) & 1u)

/* ------------------------------------------------------------------ encodeur PowerPC */

typedef struct { uint32_t *w; int n, max, err; } Em;

static void E(Em *e, uint32_t x) { if (e->n < e->max) e->w[e->n++] = x; else e->err = 1; }

#define OP_D(op, rt, ra, imm) ((uint32_t)(op) << 26 | (uint32_t)(rt) << 21 | (uint32_t)(ra) << 16 | ((uint32_t)(imm) & 0xFFFF))
#define OP_X(xo, rs, ra, rb, rc) (31u << 26 | (uint32_t)(rs) << 21 | (uint32_t)(ra) << 16 | (uint32_t)(rb) << 11 | (uint32_t)(xo) << 1 | (uint32_t)(rc))

static void addi(Em *e, int rt, int ra, int v) { E(e, OP_D(14, rt, ra, v)); }
static void addis(Em *e, int rt, int ra, int v) { E(e, OP_D(15, rt, ra, v)); }
static void ori(Em *e, int ra, int rs, unsigned v) { E(e, OP_D(24, rs, ra, v)); }
static void oris(Em *e, int ra, int rs, unsigned v) { E(e, OP_D(25, rs, ra, v)); }
static void xori(Em *e, int ra, int rs, unsigned v) { E(e, OP_D(26, rs, ra, v)); }
static void andi_(Em *e, int ra, int rs, unsigned v) { E(e, OP_D(28, rs, ra, v)); }
static void andis_(Em *e, int ra, int rs, unsigned v) { E(e, OP_D(29, rs, ra, v)); }
static void lwz(Em *e, int rt, int ra, int d) { E(e, OP_D(32, rt, ra, d)); }
static void stw(Em *e, int rs, int ra, int d) { E(e, OP_D(36, rs, ra, d)); }
static void ld(Em *e, int rt, int ra, int d) { E(e, OP_D(58, rt, ra, d & ~3)); }
static void std_(Em *e, int rs, int ra, int d) { E(e, OP_D(62, rs, ra, d & ~3)); }
static void stdu(Em *e, int rs, int ra, int d) { E(e, OP_D(62, rs, ra, (d & ~3) | 1)); }
static void x_(Em *e, int xo, int rs, int ra, int rb) { E(e, OP_X(xo, rs, ra, rb, 0)); }
static void mr(Em *e, int ra, int rs) { x_(e, 444, rs, ra, rs); }
static void extsw(Em *e, int ra, int rs) { x_(e, 986, rs, ra, 0); }
static void extsb(Em *e, int ra, int rs) { x_(e, 954, rs, ra, 0); }
static void add(Em *e, int rt, int ra, int rb) { E(e, 31u << 26 | (uint32_t)rt << 21 | (uint32_t)ra << 16 | (uint32_t)rb << 11 | 266u << 1); }
static void subf(Em *e, int rt, int ra, int rb) { E(e, 31u << 26 | (uint32_t)rt << 21 | (uint32_t)ra << 16 | (uint32_t)rb << 11 | 40u << 1); }  /* rt = rb - ra */
static void rlwinm(Em *e, int ra, int rs, int sh, int mb, int me)
{
    E(e, 21u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint32_t)(sh & 31) << 11 | (uint32_t)(mb & 31) << 6 | (uint32_t)(me & 31) << 1);
}
static void rldicl(Em *e, int ra, int rs, int sh, int mb)
{
    sh &= 63;
    E(e, 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint32_t)(sh & 31) << 11 |
         (uint32_t)(((mb & 31) << 1) | (mb >> 5)) << 5 | 0u << 2 | (uint32_t)(sh >> 5) << 1);
}
static void rldicr(Em *e, int ra, int rs, int sh, int me)
{
    sh &= 63;
    E(e, 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint32_t)(sh & 31) << 11 |
         (uint32_t)(((me & 31) << 1) | (me >> 5)) << 5 | 1u << 2 | (uint32_t)(sh >> 5) << 1);
}
static void rldimi(Em *e, int ra, int rs, int sh, int mb)
{
    sh &= 63;
    E(e, 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint32_t)(sh & 31) << 11 |
         (uint32_t)(((mb & 31) << 1) | (mb >> 5)) << 5 | 3u << 2 | (uint32_t)(sh >> 5) << 1);
}
static void mulld(Em *e, int rt, int ra, int rb) { E(e, 31u << 26 | (uint32_t)rt << 21 | (uint32_t)ra << 16 | (uint32_t)rb << 11 | 233u << 1); }
static void sradi(Em *e, int ra, int rs, int sh)
{
    E(e, 31u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint32_t)(sh & 31) << 11 | 413u << 2 | (uint32_t)((sh >> 5) & 1) << 1);
}
static void srawi(Em *e, int ra, int rs, int sh) { x_(e, 824, rs, ra, sh & 31); }
static void cmpd(Em *e, int ra, int rb) { E(e, 31u << 26 | 1u << 21 | (uint32_t)ra << 16 | (uint32_t)rb << 11); }
static void cmpld(Em *e, int ra, int rb) { E(e, 31u << 26 | 1u << 21 | (uint32_t)ra << 16 | (uint32_t)rb << 11 | 32u << 1); }
static void cmplw(Em *e, int ra, int rb) { E(e, 31u << 26 | (uint32_t)ra << 16 | (uint32_t)rb << 11 | 32u << 1); }
static void cmpdi(Em *e, int ra, int v) { E(e, 11u << 26 | 1u << 21 | (uint32_t)ra << 16 | ((uint32_t)v & 0xFFFF)); }
static void cmpwi(Em *e, int ra, int v) { E(e, 11u << 26 | (uint32_t)ra << 16 | ((uint32_t)v & 0xFFFF)); }
static void mtctr(Em *e, int rs) { E(e, 31u << 26 | (uint32_t)rs << 21 | 9u << 16 | 467u << 1); }
static void bctrl(Em *e) { E(e, 0x4E800421u); }

/* conditions sur CR0 */
enum { C_LT = 0, C_GT = 1, C_EQ = 2 };
#define BO_TRUE 12
#define BO_FALSE 4

/* branchement conditionnel en avant : renvoie l'indice a corriger */
static int bc_fwd(Em *e, int bo, int bi) { int at = e->n; E(e, 16u << 26 | (uint32_t)bo << 21 | (uint32_t)bi << 16); return at; }
static int b_fwd(Em *e) { int at = e->n; E(e, 18u << 26); return at; }
static void patch(Em *e, int at, int target)
{
    int32_t d = (target - at) * 4;
    if (at < 0 || at >= e->n) { e->err = 1; return; }
    if ((e->w[at] >> 26) == 16) {
        if (d < -32768 || d > 32767) { e->err = 1; return; }
        e->w[at] |= (uint32_t)d & 0xFFFC;
    } else e->w[at] |= (uint32_t)d & 0x3FFFFFC;
}

/* charge une constante 32 bits etendue en signe */
static void li32(Em *e, int rt, uint32_t v)
{
    int32_t s = (int32_t)v;
    if (s >= -32768 && s <= 32767) { addi(e, rt, 0, s); return; }
    addis(e, rt, 0, (int)(int16_t)(v >> 16));
    if (v & 0xFFFF) ori(e, rt, rt, v & 0xFFFF);
}

static void li64(Em *e, int rt, uint64_t v)
{
    if (v < 0x80000000u) { li32(e, rt, (uint32_t)v); return; }
    addis(e, rt, 0, (int)(int16_t)(v >> 48));
    ori(e, rt, rt, (unsigned)(v >> 32) & 0xFFFF);
    rldicr(e, rt, rt, 32, 31);
    oris(e, rt, rt, (unsigned)(v >> 16) & 0xFFFF);
    ori(e, rt, rt, (unsigned)v & 0xFFFF);
}

static void call_c(Em *e, uint64_t addr)
{
    li64(e, 12, addr);
    mtctr(e, 12);
    bctrl(e);
}

/* ------------------------------------------------------------------ registres et cadre de pile */

#define RC   31     /* VR4300 * */
#define RRAM 30     /* RDRAM */
#define RPG  29     /* table des pages de code */
#define RLIM 28     /* taille RDRAM - 8 */
#define RTAK 27     /* branchement pris */
#define RTGT 26     /* cible d'un JR */
#define RCNT 25     /* instructions MIPS faites depuis l'entree (blocs enchaines) */
#define RMAP 23     /* table des blocs */
#define RCB  22     /* adresse de la zone de code */
#define H0   14     /* r14..r21 : registres MIPS gardes dans le bloc */
#define NH   8
#define FRAME 256   /* 48 en-tete + 64 parametres + 18 sauvegardes */

#define OFF_R(i) ((int)(offsetof(VR4300, r) + 8 * (size_t)(i)))
#define OFF_HI   ((int)offsetof(VR4300, hi))
#define OFF_LO   ((int)offsetof(VR4300, lo))
#define OFF_PC   ((int)offsetof(VR4300, pc))
#define OFF_DLY  ((int)offsetof(VR4300, in_delay))
#define OFF_FCR  ((int)offsetof(VR4300, fcr31))
#define OFF_F(i) ((int)(offsetof(VR4300, fpr) + 8 * (size_t)(i)))
#define OFF_CP0(i) ((int)(offsetof(VR4300, cp0) + 8 * (size_t)(i)))

/* emplacement de sauvegarde du registre non volatil n (14..31) */
#define SAVE_SLOT(n) (112 + ((n) - 14) * 8)

static void ldptr(Em *e, int rt, int ra, int off)
{
    if (sizeof(void *) == 8) ld(e, rt, ra, off); else lwz(e, rt, ra, off);
}

/* prologue identique pour tous les blocs (longueur fixe) : un bloc peut sauter
 * directement dans le corps d'un autre (enchainement sans repasser par le C) */
static void prologue(Em *e)
{
    int i;
    E(e, 0x7C0802A6u);                 /* mflr r0 */
    std_(e, 0, 1, 16);
    stdu(e, 1, 1, -FRAME);
    for (i = 14; i <= 31; i++) std_(e, i, 1, SAVE_SLOT(i));
    mr(e, RC, 3);
    ldptr(e, RRAM, RC, (int)offsetof(VR4300, rdram));
    ldptr(e, RPG, RC, (int)offsetof(VR4300, codepage));
    lwz(e, RLIM, RC, (int)offsetof(VR4300, rdram_size));
    addi(e, RLIM, RLIM, -8);
    addi(e, RCNT, 0, 0);
    ldptr(e, RMAP, RC, (int)offsetof(VR4300, jit_map));
    ld(e, RCB, RC, (int)offsetof(VR4300, jit_code_base));
}

static int prologue_words(void)
{
    uint32_t tmp[64];
    Em e = { tmp, 0, 64, 0 };
    prologue(&e);
    return e.n;
}

static void epilogue(Em *e)
{
    int i;
    for (i = 14; i <= 31; i++) ld(e, i, 1, SAVE_SLOT(i));
    addi(e, 1, 1, FRAME);
    ld(e, 0, 1, 16);
    E(e, 0x7C0803A6u);                 /* mtlr r0 */
    E(e, 0x4E800020u);                 /* blr */
}

/* ------------------------------------------------------------------ traduction */

typedef struct {
    Em *e;
    Jit *j;
    int exits[MAX_BLOCK_INSNS * 4];   /* sauts vers les sorties (r3 deja charge) */
    int nexits;
    int chain_ret[32], nchain_ret;    /* sauts vers le retour au C (compteur deja a jour) */
    /* cache des registres MIPS dans r14..r25 */
    int host_of[32];                  /* registre hote (indice 0..11) ou -1 */
    int mips_of[NH];                  /* registre MIPS ou -1 */
    uint32_t dirty;                   /* registres MIPS modifies, pas encore ecrits en memoire */
    int age[NH], tick;
    int fr;                           /* mode FR du COP1 au moment de la traduction */
} Ctx;

static void rc_reset(Ctx *x)
{
    int i;
    for (i = 0; i < 32; i++) x->host_of[i] = -1;
    for (i = 0; i < NH; i++) x->mips_of[i] = -1;
    x->dirty = 0;
}

/* ecrit en memoire les registres modifies (sans changer l'etat suivi a la compilation) */
static void rc_writeback(Ctx *x)
{
    int m;
    for (m = 1; m < 32; m++)
        if ((x->dirty >> m & 1) && x->host_of[m] >= 0) std_(x->e, H0 + x->host_of[m], RC, OFF_R(m));
}

/* recharge tous les registres suivis (apres un appel qui a pu les modifier en memoire) */
static void rc_reload(Ctx *x)
{
    int h;
    for (h = 0; h < NH; h++) if (x->mips_of[h] > 0) ld(x->e, H0 + h, RC, OFF_R(x->mips_of[h]));
}

/* ecrit les registres modifies puis oublie tout (avant une instruction interpretee) */
static void rc_flush_drop(Ctx *x)
{
    rc_writeback(x);
    rc_reset(x);
}

static int rc_alloc(Ctx *x, int m)
{
    int h, best = -1;
    for (h = 0; h < NH; h++) if (x->mips_of[h] < 0) { best = h; break; }
    if (best < 0) {                                   /* le moins recemment utilise */
        best = 0;
        for (h = 1; h < NH; h++) if (x->age[h] < x->age[best]) best = h;
        {
            int old = x->mips_of[best];
            if (x->dirty >> old & 1) { std_(x->e, H0 + best, RC, OFF_R(old)); x->dirty &= ~(1u << old); }
            x->host_of[old] = -1;
        }
    }
    x->mips_of[best] = m;
    x->host_of[m] = best;
    x->age[best] = ++x->tick;
    return best;
}

/* registre hote contenant la valeur du registre MIPS m (tmp sert pour $zero) */
static int R_rd(Ctx *x, int m, int tmp)
{
    int h;
    if (m == 0) { addi(x->e, tmp, 0, 0); return tmp; }
    h = x->host_of[m];
    if (h < 0) { h = rc_alloc(x, m); ld(x->e, H0 + h, RC, OFF_R(m)); }
    x->age[h] = ++x->tick;
    return H0 + h;
}

/* registre hote ou ecrire le registre MIPS m (tmp si m = $zero : resultat jete) */
static int R_wr(Ctx *x, int m, int tmp)
{
    int h;
    if (m == 0) return tmp;
    h = x->host_of[m];
    if (h < 0) h = rc_alloc(x, m);
    x->age[h] = ++x->tick;
    x->dirty |= 1u << m;
    return H0 + h;
}

/* sortie anticipee (registres deja ecrits en memoire) : r3 = instructions faites */
static void exit_now(Ctx *x, int count)
{
    li32(x->e, 3, (uint32_t)count);
    x->exits[x->nexits++] = b_fwd(x->e);
}

/* r11 : registre de travail qui ne porte jamais d'operande */
static void set_pc(Ctx *x, uint32_t pc)
{
    li32(x->e, 11, pc);
    stw(x->e, 11, RC, OFF_PC);
}

static void set_delay(Ctx *x, int on)
{
    addi(x->e, 11, 0, on);
    stw(x->e, 11, RC, OFF_DLY);
}

/* instruction interpretee (aide C). Sortie si exception. */
static void emit_exec1(Ctx *x, uint32_t ins, uint32_t pc, int idx, int in_delay)
{
    Em *e = x->e;
    int skip;
    rc_flush_drop(x);                               /* l'interpreteur lit et ecrit la memoire */
    set_pc(x, pc);
    if (in_delay) set_delay(x, 1);
    mr(e, 3, RC);
    li32(e, 4, ins);
    call_c(e, x->j->h_exec1);
    if (in_delay) { mr(e, 5, 3); set_delay(x, 0); mr(e, 3, 5); }
    cmpwi(e, 3, 0);
    skip = bc_fwd(e, BO_TRUE, C_EQ);
    exit_now(x, idx + 1);
    patch(e, skip, e->n);
}

/* chargement / rangement avec chemin rapide KSEG0/KSEG1 en RDRAM */
/* garde : mode FR identique a la traduction, sinon chemin lent (l'interpreteur, comme
 * la plupart des emulateurs, ne verifie pas le bit CU1 : on fait pareil) */
static int fp_guard(Ctx *x, int *sl)
{
    Em *e = x->e;
    ld(e, 9, RC, OFF_CP0(CP0_STATUS));
    andis_(e, 10, 9, 0x0400);
    sl[0] = bc_fwd(e, BO_TRUE, x->fr ? C_EQ : C_GT);     /* andis. : resultat >= 0, donc GT = non nul */
    return 1;
}
/* registre flottant 32 bits r : mot double qui le contient, et moitie haute ou non */
static int fw_off(Ctx *x, int r) { return OFF_F(x->fr ? r : (r & ~1)); }
static int fw_hi(Ctx *x, int r) { return !x->fr && (r & 1); }
static int fd_off(Ctx *x, int r) { return OFF_F(x->fr ? r : (r & ~1)); }
/* lit le mot 32 bits du registre flottant r dans dst (bits bas, haut indefini) */
static void fw_read(Ctx *x, int dst, int r)
{
    ld(x->e, dst, RC, fw_off(x, r));
    if (fw_hi(x, r)) rldicl(x->e, dst, dst, 32, 32);
}
/* ecrit les 32 bits bas de src dans le registre flottant r (utilise r8) */
static void fw_write(Ctx *x, int src, int r)
{
    ld(x->e, 8, RC, fw_off(x, r));
    if (fw_hi(x, r)) rldimi(x->e, 8, src, 32, 0); else rldimi(x->e, 8, src, 0, 32);
    std_(x->e, 8, RC, fw_off(x, r));
}

static void emit_mem(Ctx *x, uint32_t ins, uint32_t pc, int idx, int in_delay, int store, int size, int sgn, int fp)
{
    Em *e = x->e;
    int rs = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31);
    int16_t off = (int16_t)(ins & 0xFFFF);
    int to_slow[6], ns = 0, done, k, skip, hb, hv = 7, hd = 7;
    /* toutes les allocations de registres AVANT le premier branchement vers le
     * chemin lent : les deux chemins partagent ainsi le meme etat du cache */
    hb = R_rd(x, rs, 4);
    if (fp) {
        /* registres flottants en memoire ; chemin rapide si le mode FR n'a pas change */
        ns = fp_guard(x, to_slow);
        if (store) { if (size == 4) fw_read(x, 7, rt); else ld(e, 7, RC, fd_off(x, rt)); hv = 7; }
    } else if (store) hv = R_rd(x, rt, 7);
    else if (rt) {
        int h = x->host_of[rt];
        if (h < 0) h = rc_alloc(x, rt);                    /* sans chargement ni marque "modifie" */
        x->age[h] = ++x->tick;
        hd = H0 + h;
    }
    addi(e, 4, hb, off);                        /* adresse virtuelle (32 bits bas) */
    rlwinm(e, 5, 4, 2, 30, 31);                 /* 2 bits de poids fort */
    cmpwi(e, 5, 2);
    to_slow[ns++] = bc_fwd(e, BO_FALSE, C_EQ);  /* pas KSEG0/1 */
    if (size > 1) {
        andi_(e, 5, 4, (unsigned)(size - 1));
        to_slow[ns++] = bc_fwd(e, BO_FALSE, C_EQ);   /* non aligne */
    }
    rlwinm(e, 6, 4, 0, 3, 31);                  /* adresse physique */
    cmplw(e, 6, RLIM);
    to_slow[ns++] = bc_fwd(e, BO_TRUE, C_GT);   /* hors RDRAM */
    if (!store && fp) {
        if (size == 4) { x_(e, 23, 7, RRAM, 6); fw_write(x, 7, rt); }
        else { x_(e, 21, 7, RRAM, 6); std_(e, 7, RC, fd_off(x, rt)); }
    } else if (!store) {
        switch (size) {
        case 1: x_(e, 87, hd, RRAM, 6); if (sgn) extsb(e, hd, hd); break;          /* lbzx */
        case 2: x_(e, sgn ? 343 : 279, hd, RRAM, 6); break;                        /* lhax / lhzx */
        case 4: x_(e, 23, hd, RRAM, 6); if (sgn) extsw(e, hd, hd); break;          /* lwzx */
        default: x_(e, 21, hd, RRAM, 6); break;                                    /* ldx */
        }
    } else {
        rlwinm(e, 8, 6, 20, 12, 31);            /* page de 4 Ko */
        x_(e, 87, 9, RPG, 8);                   /* lbzx : page contenant du code ? */
        cmpwi(e, 9, 0);
        to_slow[ns++] = bc_fwd(e, BO_FALSE, C_EQ);
        switch (size) {
        case 1: x_(e, 215, hv, RRAM, 6); break;  /* stbx */
        case 2: x_(e, 407, hv, RRAM, 6); break;  /* sthx */
        case 4: x_(e, 151, hv, RRAM, 6); break;  /* stwx */
        default: x_(e, 149, hv, RRAM, 6); break; /* stdx */
        }
    }
    done = b_fwd(e);
    /* chemin lent : l'interpreteur (exceptions, TLB, registres materiels, page de code) */
    for (k = 0; k < ns; k++) patch(e, to_slow[k], e->n);
    rc_writeback(x);
    set_pc(x, pc);
    if (in_delay) set_delay(x, 1);
    mr(e, 3, RC);
    if (fp) {
        li32(e, 4, ins);                        /* l'interpreteur fait toute l'instruction */
        call_c(e, x->j->h_exec1);
    } else {
        rlwinm(e, 4, 4, 0, 0, 31);              /* adresse 32 bits */
        li32(e, 5, (uint32_t)rt | (uint32_t)size << 8 | (uint32_t)sgn << 12);
        call_c(e, store ? x->j->h_store : x->j->h_load);
    }
    if (in_delay) { mr(e, 5, 3); set_delay(x, 0); mr(e, 3, 5); }
    cmpwi(e, 3, 0);
    skip = bc_fwd(e, BO_TRUE, C_EQ);
    exit_now(x, idx + 1);
    patch(e, skip, e->n);
    if (store && !in_delay) {
        /* une page de code a peut-etre ete modifiee : on sort pour ne pas executer du vieux code */
        set_pc(x, pc + 4);
        exit_now(x, idx + 1);
    }
    rc_reload(x);                               /* l'aide a pu ecrire c->r[rt] */
    patch(e, done, e->n);
    if (!store && !fp && rt) x->dirty |= 1u << rt;
}

/* MFC1 / DMFC1 / MTC1 / DMTC1 : en ligne si FR = 1 et COP1 utilisable, sinon interpreteur */
static void emit_fpmove(Ctx *x, uint32_t ins, uint32_t pc, int idx, int in_delay)
{
    Em *e = x->e;
    int fmt = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31), fs = (int)((ins >> 11) & 31);
    int sl[2], done, skip, h = 7;
    if (fmt == 0 || fmt == 1) {                 /* allocation de rt avant le branchement (etat commun) */
        if (rt) { int k = x->host_of[rt]; if (k < 0) k = rc_alloc(x, rt); x->age[k] = ++x->tick; h = H0 + k; }
    } else h = R_rd(x, rt, 7);
    fp_guard(x, sl);
    switch (fmt) {
    case 0: fw_read(x, 8, fs); extsw(e, h, 8); break;                 /* MFC1 */
    case 1: ld(e, h, RC, fd_off(x, fs)); break;                       /* DMFC1 */
    case 4: fw_write(x, h, fs); break;                                /* MTC1 */
    default: std_(e, h, RC, fd_off(x, fs)); break;                    /* DMTC1 */
    }
    done = b_fwd(e);
    patch(e, sl[0], e->n);
    rc_writeback(x);
    set_pc(x, pc);
    if (in_delay) set_delay(x, 1);
    mr(e, 3, RC);
    li32(e, 4, ins);
    call_c(e, x->j->h_exec1);
    if (in_delay) { mr(e, 5, 3); set_delay(x, 0); mr(e, 3, 5); }
    cmpwi(e, 3, 0);
    skip = bc_fwd(e, BO_TRUE, C_EQ);
    exit_now(x, idx + 1);
    patch(e, skip, e->n);
    rc_reload(x);
    patch(e, done, e->n);
    if ((fmt == 0 || fmt == 1) && rt) x->dirty |= 1u << rt;
}

/* ADD SUB DADD DSUB ADDI DADDI : calcul en ligne, et si debordement l'interpreteur
 * refait l'instruction (il declenche l'exception exactement comme le vrai processeur) */
static int is_trap_arith(uint32_t ins)
{
    uint32_t op = ins >> 26, fn = ins & 63;
    if (op == 0x08 || op == 0x18) return 1;
    return op == 0 && ((ins >> 6) & 31) == 0 && (fn == 0x20 || fn == 0x22 || fn == 0x2C || fn == 0x2E);
}

static void emit_trap_arith(Ctx *x, uint32_t ins, uint32_t pc, int idx, int in_delay)
{
    Em *e = x->e;
    uint32_t op = ins >> 26, fn = ins & 63;
    int rs = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31), rd, a, b, d = 6, ov, done, skip;
    int wide, sub = 0;
    if (op) { rd = rt; wide = op == 0x18; }
    else { rd = (int)((ins >> 11) & 31); wide = fn >= 0x2C; sub = fn == 0x22 || fn == 0x2E; }
    a = R_rd(x, rs, 4);
    if (op) { addi(e, 5, 0, (int16_t)(ins & 0xFFFF)); b = 5; } else b = R_rd(x, rt, 5);
    if (rd) { int k = x->host_of[rd]; if (k < 0) k = rc_alloc(x, rd); x->age[k] = ++x->tick; d = H0 + k; }
    if (!wide) {
        extsw(e, 7, a); extsw(e, 8, b);
        if (sub) subf(e, 6, 8, 7); else add(e, 6, 7, 8);
        extsw(e, 7, 6);
        cmpd(e, 7, 6);
        ov = bc_fwd(e, BO_FALSE, C_EQ);
    } else {
        if (sub) { subf(e, 6, b, a); E(e, 31u << 26 | (uint32_t)a << 21 | 7u << 16 | (uint32_t)b << 11 | 316u << 1); }  /* xor 7,a,b */
        else { add(e, 6, a, b); E(e, 31u << 26 | (uint32_t)b << 21 | 7u << 16 | 6u << 11 | 316u << 1); }                                  /* xor 7,b,r */
        E(e, 31u << 26 | (uint32_t)a << 21 | 8u << 16 | 6u << 11 | 316u << 1);                                                               /* xor 8,a,r */
        E(e, 31u << 26 | 7u << 21 | 7u << 16 | 8u << 11 | 28u << 1);                                                                         /* and 7,7,8 */
        cmpdi(e, 7, 0);
        ov = bc_fwd(e, BO_TRUE, C_LT);
    }
    if (!wide) extsw(e, d, 6); else mr(e, d, 6);
    done = b_fwd(e);
    patch(e, ov, e->n);
    rc_writeback(x);
    set_pc(x, pc);
    if (in_delay) set_delay(x, 1);
    mr(e, 3, RC);
    li32(e, 4, ins);
    call_c(e, x->j->h_exec1);
    if (in_delay) { mr(e, 5, 3); set_delay(x, 0); mr(e, 3, 5); }
    cmpwi(e, 3, 0);
    skip = bc_fwd(e, BO_TRUE, C_EQ);
    exit_now(x, idx + 1);
    patch(e, skip, e->n);
    rc_reload(x);
    patch(e, done, e->n);
    if (rd) x->dirty |= 1u << rd;
}

/* calcul flottant (formats S D W L) et CFC1/CTC1 : l'interpreteur ne touche alors ni la
 * memoire ni les registres entiers (sauf rt pour CFC1/CTC1) et ne declenche aucune
 * exception, et r14..r21 sont preserves par l'appel C (ABI) : pas besoin de vider le cache */
static int is_fp_light(uint32_t ins)
{
    int fmt = (int)((ins >> 21) & 31);
    if ((ins >> 26) != 0x11) return 0;
    return fmt == 0x10 || fmt == 0x11 || fmt == 0x14 || fmt == 0x15 || ((fmt == 2 || fmt == 6) && (ins & 0x7FF) == 0);
}

static void emit_fp_light(Ctx *x, uint32_t ins)
{
    Em *e = x->e;
    int fmt = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31), h;
    if (fmt == 6 && rt && (h = x->host_of[rt]) >= 0 && (x->dirty >> rt & 1)) {   /* CTC1 lit rt en memoire */
        std_(e, H0 + h, RC, OFF_R(rt));
        x->dirty &= ~(1u << rt);
    }
    mr(e, 3, RC);
    li32(e, 4, ins);
    call_c(e, x->j->h_exec1);
    if (fmt == 2 && rt && (h = x->host_of[rt]) >= 0) {                            /* CFC1 a ecrit rt en memoire */
        x->dirty &= ~(1u << rt);
        ld(e, H0 + h, RC, OFF_R(rt));
    }
}

static int is_fpmove(uint32_t ins)
{
    int fmt = (int)((ins >> 21) & 31);
    return (ins >> 26) == 0x11 && (fmt == 0 || fmt == 1 || fmt == 4 || fmt == 5) && (ins & 0x7FF) == 0;
}

/* 1 = instruction traduite en ligne, 0 = pas une instruction "simple" */
static int emit_alu(Ctx *x, uint32_t ins)
{
    Em *e = x->e;
    int op = (int)(ins >> 26), rs = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31), rd = (int)((ins >> 11) & 31);
    int sa = (int)((ins >> 6) & 31), fn = (int)(ins & 63);
    int16_t simm = (int16_t)(ins & 0xFFFF);
    unsigned uimm = ins & 0xFFFF;
    int a, b, d, sk;
    if (op == 0) {
        switch (fn) {
        case 0x00: case 0x02: case 0x03:                       /* SLL SRL SRA */
            if (ins == 0 || !rd) return 1;                      /* NOP */
            a = R_rd(x, rt, 4); d = R_wr(x, rd, 4);
            if (fn == 0x00) rlwinm(e, d, a, sa, 0, 31 - sa);
            else if (fn == 0x02) rlwinm(e, d, a, (32 - sa) & 31, sa, 31);
            else srawi(e, d, a, sa);
            extsw(e, d, d);
            return 1;
        case 0x04: case 0x06: case 0x07:                       /* SLLV SRLV SRAV */
            if (!rd) return 1;
            a = R_rd(x, rs, 5); andi_(e, 5, a, 31);
            b = R_rd(x, rt, 4); d = R_wr(x, rd, 4);
            x_(e, fn == 0x04 ? 24 : fn == 0x06 ? 536 : 792, b, d, 5);
            extsw(e, d, d);
            return 1;
        case 0x14: case 0x16: case 0x17:                       /* DSLLV DSRLV DSRAV */
            if (!rd) return 1;
            a = R_rd(x, rs, 5); andi_(e, 5, a, 63);
            b = R_rd(x, rt, 4); d = R_wr(x, rd, 4);
            x_(e, fn == 0x14 ? 27 : fn == 0x16 ? 539 : 794, b, d, 5);
            return 1;
        case 0x0F: return 1;                                    /* SYNC */
        case 0x10: case 0x12:                                   /* MFHI MFLO */
            if (!rd) return 1;
            d = R_wr(x, rd, 4);
            ld(e, d, RC, fn == 0x10 ? OFF_HI : OFF_LO);
            return 1;
        case 0x11: case 0x13:                                   /* MTHI MTLO */
            a = R_rd(x, rs, 4);
            std_(e, a, RC, fn == 0x11 ? OFF_HI : OFF_LO);
            return 1;
        case 0x18: case 0x19: {                                 /* MULT MULTU */
            a = R_rd(x, rs, 4); b = R_rd(x, rt, 5);
            if (fn == 0x18) { extsw(e, 4, a); extsw(e, 5, b); }
            else { rldicl(e, 4, a, 0, 32); rldicl(e, 5, b, 0, 32); }
            mulld(e, 6, 4, 5);
            extsw(e, 7, 6); std_(e, 7, RC, OFF_LO);
            sradi(e, 7, 6, 32); extsw(e, 7, 7); std_(e, 7, RC, OFF_HI);
            return 1;
        }
        case 0x21: case 0x23: case 0x2D: case 0x2F:            /* ADDU SUBU DADDU DSUBU */
        case 0x24: case 0x25: case 0x26: case 0x27:            /* AND OR XOR NOR */
        case 0x2A: case 0x2B:                                   /* SLT SLTU */
            if (!rd) return 1;
            a = R_rd(x, rs, 4); b = R_rd(x, rt, 5);
            if (fn == 0x2A || fn == 0x2B) {
                if (fn == 0x2A) cmpd(e, a, b); else cmpld(e, a, b);
                d = R_wr(x, rd, 4);
                addi(e, d, 0, 0);
                sk = bc_fwd(e, BO_FALSE, C_LT);
                addi(e, d, 0, 1);
                patch(e, sk, e->n);
                return 1;
            }
            d = R_wr(x, rd, 4);
            switch (fn) {
            case 0x21: add(e, d, a, b); extsw(e, d, d); break;
            case 0x23: subf(e, d, b, a); extsw(e, d, d); break;
            case 0x2D: add(e, d, a, b); break;
            case 0x2F: subf(e, d, b, a); break;
            case 0x24: x_(e, 28, a, d, b); break;
            case 0x25: x_(e, 444, a, d, b); break;
            case 0x26: x_(e, 316, a, d, b); break;
            default:   x_(e, 124, a, d, b); break;
            }
            return 1;
        case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:   /* DSLL DSRL DSRA (+32) */
        {
            int s = sa + (fn >= 0x3C ? 32 : 0), k = fn & ~4;
            if (!rd) return 1;
            a = R_rd(x, rt, 4); d = R_wr(x, rd, 4);
            if (k == 0x38) { if (s) rldicr(e, d, a, s, 63 - s); else mr(e, d, a); }
            else if (k == 0x3A) { if (s) rldicl(e, d, a, 64 - s, s); else mr(e, d, a); }
            else sradi(e, d, a, s);
            return 1;
        }
        }
        return 0;
    }
    switch (op) {
    case 0x09: case 0x19:                                       /* ADDIU DADDIU */
        if (!rt) return 1;
        a = R_rd(x, rs, 4); d = R_wr(x, rt, 4);
        addi(e, d, a == 0 ? 4 : a, simm);                       /* (r0 vaudrait 0 en addi : jamais utilise) */
        if (op == 0x09) extsw(e, d, d);
        return 1;
    case 0x0A: case 0x0B:                                       /* SLTI SLTIU */
        if (!rt) return 1;
        a = R_rd(x, rs, 4);
        if (op == 0x0A) cmpdi(e, a, simm);
        else { addi(e, 5, 0, simm); cmpld(e, a, 5); }
        d = R_wr(x, rt, 4);
        addi(e, d, 0, 0);
        sk = bc_fwd(e, BO_FALSE, C_LT);
        addi(e, d, 0, 1);
        patch(e, sk, e->n);
        return 1;
    case 0x0C: case 0x0D: case 0x0E:                            /* ANDI ORI XORI */
        if (!rt) return 1;
        a = R_rd(x, rs, 4); d = R_wr(x, rt, 4);
        if (op == 0x0C) andi_(e, d, a, uimm);
        else if (op == 0x0D) ori(e, d, a, uimm);
        else xori(e, d, a, uimm);
        return 1;
    case 0x0F:                                                  /* LUI */
        if (!rt) return 1;
        d = R_wr(x, rt, 4);
        addis(e, d, 0, simm);
        return 1;
    }
    return 0;
}

/* acces memoire traduits : renvoie 1 et la description */
static int mem_kind(uint32_t ins, int *store, int *size, int *sgn, int *fp)
{
    *fp = 0;
    switch (ins >> 26) {
    case 0x31: *store = 0; *size = 4; *sgn = 0; *fp = 1; return 1;   /* LWC1 */
    case 0x35: *store = 0; *size = 8; *sgn = 0; *fp = 1; return 1;   /* LDC1 */
    case 0x39: *store = 1; *size = 4; *sgn = 0; *fp = 1; return 1;   /* SWC1 */
    case 0x3D: *store = 1; *size = 8; *sgn = 0; *fp = 1; return 1;   /* SDC1 */
    case 0x20: *store = 0; *size = 1; *sgn = 1; return 1;  /* LB */
    case 0x24: *store = 0; *size = 1; *sgn = 0; return 1;  /* LBU */
    case 0x21: *store = 0; *size = 2; *sgn = 1; return 1;  /* LH */
    case 0x25: *store = 0; *size = 2; *sgn = 0; return 1;  /* LHU */
    case 0x23: *store = 0; *size = 4; *sgn = 1; return 1;  /* LW */
    case 0x27: *store = 0; *size = 4; *sgn = 0; return 1;  /* LWU */
    case 0x37: *store = 0; *size = 8; *sgn = 0; return 1;  /* LD */
    case 0x28: *store = 1; *size = 1; *sgn = 0; return 1;  /* SB */
    case 0x29: *store = 1; *size = 2; *sgn = 0; return 1;  /* SH */
    case 0x2B: *store = 1; *size = 4; *sgn = 0; return 1;  /* SW */
    case 0x3F: *store = 1; *size = 8; *sgn = 0; return 1;  /* SD */
    }
    return 0;
}

/* branchements : 1 si c'en est un (et description) */
typedef struct { int kind, likely, link; uint32_t target; } Br;   /* kind : 1 cond, 2 J, 3 JR */

static int is_branch(uint32_t ins, uint32_t pc, Br *b)
{
    int op = (int)(ins >> 26), rt = (int)((ins >> 16) & 31), fn = (int)(ins & 63);
    uint32_t rel = pc + 4 + (uint32_t)((int32_t)(int16_t)(ins & 0xFFFF) * 4);
    memset(b, 0, sizeof *b);
    switch (op) {
    case 0x00:
        if (fn == 0x08) { b->kind = 3; return 1; }
        if (fn == 0x09) { b->kind = 3; b->link = 1; return 1; }
        return 0;
    case 0x01:
        if (rt == 0 || rt == 1 || rt == 2 || rt == 3 || rt == 16 || rt == 17 || rt == 18 || rt == 19) {
            b->kind = 1; b->likely = (rt & 2) != 0; b->link = rt >= 16; b->target = rel; return 1;
        }
        return 0;
    case 0x02: case 0x03:
        b->kind = 2; b->link = op == 3; b->target = ((pc + 4) & 0xF0000000u) | ((ins & 0x03FFFFFF) << 2); return 1;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x14: case 0x15: case 0x16: case 0x17:
        b->kind = 1; b->likely = op >= 0x14; b->target = rel; return 1;
    case 0x11:
        if (((ins >> 21) & 31) == 8) { b->kind = 1; b->likely = (rt & 2) != 0; b->target = rel; return 1; }   /* BC1x */
        return 0;
    }
    return 0;
}

/* instructions qui doivent finir le bloc AVANT elles (laissees a l'interpreteur) */
static int is_barrier(uint32_t ins)
{
    int op = (int)(ins >> 26), fn = (int)(ins & 63);
    if (op == 0x00 && (fn == 0x0C || fn == 0x0D || (fn >= 0x30 && fn <= 0x36))) return 1;   /* SYSCALL BREAK TRAP */
    if (op == 0x01 && ((ins >> 16) & 31) >= 8 && ((ins >> 16) & 31) <= 14) return 1;          /* TRAPI */
    if (op == 0x10) return 1;                                                                   /* COP0 (ERET, MTC0, TLB) */
    if (op == 0x2F) return 1;                                                                   /* CACHE */
    return 0;
}

/* r27 = 1 si la condition du branchement est vraie */
static void emit_cond(Ctx *x, uint32_t ins)
{
    Em *e = x->e;
    int op = (int)(ins >> 26), rs = (int)((ins >> 21) & 31), rt = (int)((ins >> 16) & 31), sk = 0, a, b;
    if (op == 0x11) {                                           /* BC1F / BC1T */
        lwz(e, 4, RC, OFF_FCR);
        rlwinm(e, 4, 4, 9, 31, 31);                             /* bit 23 */
        addi(e, RTAK, 0, 0);
        cmpwi(e, 4, 0);
        sk = bc_fwd(e, (rt & 1) ? BO_TRUE : BO_FALSE, C_EQ);
    } else {
        a = R_rd(x, rs, 4);
        switch (op == 1 ? (rt & 1 ? 0x101 : 0x100) : (op & ~0x10)) {
        case 0x04: b = R_rd(x, rt, 5); cmpd(e, a, b); addi(e, RTAK, 0, 0); sk = bc_fwd(e, BO_FALSE, C_EQ); break;   /* BEQ */
        case 0x05: b = R_rd(x, rt, 5); cmpd(e, a, b); addi(e, RTAK, 0, 0); sk = bc_fwd(e, BO_TRUE, C_EQ); break;    /* BNE */
        case 0x06: cmpdi(e, a, 0); addi(e, RTAK, 0, 0); sk = bc_fwd(e, BO_TRUE, C_GT); break;                         /* BLEZ */
        case 0x07: cmpdi(e, a, 0); addi(e, RTAK, 0, 0); sk = bc_fwd(e, BO_FALSE, C_GT); break;                        /* BGTZ */
        case 0x100: cmpdi(e, a, 0); addi(e, RTAK, 0, 0); sk = bc_fwd(e, BO_FALSE, C_LT); break;                       /* BLTZ */
        case 0x101: cmpdi(e, a, 0); addi(e, RTAK, 0, 0); sk = bc_fwd(e, BO_TRUE, C_LT); break;                        /* BGEZ */
        default: e->err = 1; return;
        }
    }
    addi(e, RTAK, 0, 1);
    patch(e, sk, e->n);
}

/* enchainement : si le budget le permet et qu'aucune interruption n'attend, on
 * cherche le bloc suivant dans la table et on saute directement dans son corps.
 * Sinon retour au C (dispatcher), qui fera le reste. */
static void emit_chain(Ctx *x, int has_static, uint32_t target, Jit *j)
{
    Em *e = x->e;
    int pw = prologue_words();
    /* budget relu en memoire : les aides C le mettent a 0 quand une interruption arrive */
    lwz(e, 9, RC, (int)offsetof(VR4300, jit_budget));
    cmplw(e, RCNT, 9);
    x->chain_ret[x->nchain_ret++] = bc_fwd(e, BO_FALSE, C_LT);
    if (has_static && (target & 0xC0000000u) == 0x80000000u && ((target & 0x1FFFFFFF) >> PAGE_SHIFT) < j->npages &&
        ((target & 0x1FFFFFFF) >> PAGE_SHIFT) * sizeof(void *) < 32768) {
        /* cible connue : adresses de table constantes */
        uint32_t pa = target & 0x1FFFFFFF, pg = pa >> PAGE_SHIFT;
        ldptr(e, 8, RMAP, (int)(pg * sizeof(void *)));
        cmpdi(e, 8, 0);
        x->chain_ret[x->nchain_ret++] = bc_fwd(e, BO_TRUE, C_EQ);
        lwz(e, 10, 8, (int)(pa & 0xFFC));
    } else {
        lwz(e, 4, RC, OFF_PC);
        rlwinm(e, 5, 4, 2, 30, 31);
        cmpwi(e, 5, 2);
        x->chain_ret[x->nchain_ret++] = bc_fwd(e, BO_FALSE, C_EQ);    /* hors KSEG0/1 */
        rlwinm(e, 6, 4, 0, 3, 31);                                     /* adresse physique */
        rlwinm(e, 7, 6, 20, 12, 31);                                   /* page */
        lwz(e, 8, RC, (int)offsetof(VR4300, jit_npages));
        cmplw(e, 7, 8);
        x->chain_ret[x->nchain_ret++] = bc_fwd(e, BO_FALSE, C_LT);
        if (sizeof(void *) == 8) { rldicr(e, 7, 7, 3, 60); x_(e, 21, 8, RMAP, 7); }    /* ldx */
        else { rlwinm(e, 7, 7, 2, 0, 29); x_(e, 23, 8, RMAP, 7); }                   /* lwzx */
        cmpdi(e, 8, 0);
        x->chain_ret[x->nchain_ret++] = bc_fwd(e, BO_TRUE, C_EQ);     /* page sans blocs */
        rlwinm(e, 9, 6, 0, 20, 29);                                    /* (pa & 0xFFC) = case * 4 */
        x_(e, 23, 10, 8, 9);                                           /* lwzx : entree */
    }
    andi_(e, 9, 10, 1);
    x->chain_ret[x->nchain_ret++] = bc_fwd(e, BO_FALSE, C_EQ);        /* boucle d'attente : au C */
    cmpwi(e, 10, 0);
    x->chain_ret[x->nchain_ret++] = bc_fwd(e, BO_TRUE, C_EQ);         /* pas encore compile */
    rlwinm(e, 10, 10, 1, 0, 29);                                       /* (entree >> 1) * 4 */
    addi(e, 10, 10, pw * 4 - 4);                                       /* - 4 + prologue */
    add(e, 10, RCB, 10);
    mtctr(e, 10);
    E(e, 0x4E800420u);                                                 /* bctr */
}

/* traduit le bloc qui commence a pc ; renvoie le nombre de mots, 0 si impossible */
static int compile_block(Jit *j, VR4300 *c, uint32_t pc, uint32_t *out, int max, int *idle)
{
    Em em = { out, 0, max, 0 };
    Ctx x;
    uint32_t pa0 = pc & 0x1FFFFFFF, page_end = (pa0 | 0xFFF) + 1, p = pc;
    int idx = 0, k, ended = 0, ret, chain, has_static = 0;
    uint32_t static_target = 0;
    memset(&x, 0, sizeof x);
    x.e = &em; x.j = j;
    x.fr = (c->cp0[CP0_STATUS] & 0x04000000u) != 0;
    rc_reset(&x);
    *idle = 0;
    prologue(&em);
    while (!ended && idx < MAX_BLOCK_INSNS && !em.err) {
        uint32_t pa = p & 0x1FFFFFFF, ins;
        Br br;
        int store, size, sgn, fp;
        if (pa >= page_end || pa + 4 > c->rdram_size) break;
        ins = (uint32_t)c->rdram[pa] << 24 | (uint32_t)c->rdram[pa + 1] << 16 | (uint32_t)c->rdram[pa + 2] << 8 | c->rdram[pa + 3];
        if (is_barrier(ins)) {
            if (idx == 0) return 0;                             /* l'interpreteur s'en charge */
            break;
        }
        if (is_branch(ins, p, &br)) {
            uint32_t slot, spa = pa + 4;
            Br b2;
            int s_store, s_size, s_sgn, s_fp, notaken;
            if (spa >= page_end || spa + 4 > c->rdram_size) break;   /* creneau sur la page suivante */
            slot = (uint32_t)c->rdram[spa] << 24 | (uint32_t)c->rdram[spa + 1] << 16 | (uint32_t)c->rdram[spa + 2] << 8 | c->rdram[spa + 3];
            if (is_branch(slot, p + 4, &b2) || is_barrier(slot)) { if (idx == 0) return 0; break; }
            /* condition et cible evaluees AVANT le creneau */
            if (br.kind == 1) emit_cond(&x, ins);
            else addi(&em, RTAK, 0, 1);
            if (br.kind == 3) { int t = R_rd(&x, (int)((ins >> 21) & 31), 4); mr(&em, RTGT, t); }
            if (br.likely) {
                int sk;
                cmpwi(&em, RTAK, 0);
                sk = bc_fwd(&em, BO_FALSE, C_EQ);
                rc_writeback(&x);
                set_pc(&x, p + 8);                               /* non pris : creneau annule */
                exit_now(&x, idx + 1);
                patch(&em, sk, em.n);
            }
            if (br.link) {
                int rl = br.kind == 3 ? (int)((ins >> 11) & 31) : 31;
                if (rl) { int d = R_wr(&x, rl, 4); li32(&em, d, p + 8); }
            }
            /* creneau de retard */
            if (mem_kind(slot, &s_store, &s_size, &s_sgn, &s_fp)) emit_mem(&x, slot, p + 4, idx + 1, 1, s_store, s_size, s_sgn, s_fp);
            else if (is_fpmove(slot)) emit_fpmove(&x, slot, p + 4, idx + 1, 1);
            else if (is_fp_light(slot)) emit_fp_light(&x, slot);
            else if (is_trap_arith(slot)) emit_trap_arith(&x, slot, p + 4, idx + 1, 1);
            else if (!emit_alu(&x, slot)) emit_exec1(&x, slot, p + 4, idx + 1, 1);
            /* nouveau pc */
            rc_writeback(&x);
            cmpwi(&em, RTAK, 0);
            if (br.kind == 2 || (br.kind == 1 && br.target == p + 8)) {
                /* saut inconditionnel (ou cible = suite) : cible connue a la compilation */
                li32(&em, 4, br.kind == 2 ? br.target : p + 8);
                stw(&em, 4, RC, OFF_PC);
                has_static = 1; static_target = br.kind == 2 ? br.target : p + 8;
            } else {
                /* deux issues : on enchaine directement chacune (cible connue) */
                notaken = bc_fwd(&em, BO_TRUE, C_EQ);
                if (br.kind == 3) { stw(&em, RTGT, RC, OFF_PC); li32(&em, 3, (uint32_t)(idx + 2)); add(&em, RCNT, RCNT, 3); emit_chain(&x, 0, 0, j); }
                else { li32(&em, 4, br.target); stw(&em, 4, RC, OFF_PC); li32(&em, 3, (uint32_t)(idx + 2)); add(&em, RCNT, RCNT, 3); emit_chain(&x, 1, br.target, j); }
                patch(&em, notaken, em.n);
                li32(&em, 4, p + 8);
                stw(&em, 4, RC, OFF_PC);
                has_static = 1; static_target = p + 8;
            }
            li32(&em, 3, (uint32_t)(idx + 2));
            /* boucle d'attente ? (bloc qui revient sur lui-meme) */
            if (br.kind != 3 && br.target == pc && c->idle_skip && vr4300_idle_loop(c, pc, p)) *idle = 1;
            idx += 2;
            ended = 1;
            break;
        }
        if (mem_kind(ins, &store, &size, &sgn, &fp)) emit_mem(&x, ins, p, idx, 0, store, size, sgn, fp);
        else if (is_fpmove(ins)) emit_fpmove(&x, ins, p, idx, 0);
        else if (is_fp_light(ins)) emit_fp_light(&x, ins);
        else if (is_trap_arith(ins)) emit_trap_arith(&x, ins, p, idx, 0);
        else if (!emit_alu(&x, ins)) {
            emit_exec1(&x, ins, p, idx, 0);
            j->fallbacks++;
        }
        idx++;
        p += 4;
    }
    if (idx == 0) return 0;
    if (!ended) { rc_writeback(&x); set_pc(&x, p); li32(&em, 3, (uint32_t)idx); has_static = 1; static_target = p; }
    /* fin normale : pc et r3 a jour. On compte, puis on tente d'enchainer le bloc suivant */
    add(&em, RCNT, RCNT, 3);
    chain = em.n;
    emit_chain(&x, has_static, static_target, j);
    /* sorties anticipees (r3 = instructions faites dans CE bloc) */
    for (k = 0; k < x.nexits; k++) patch(&em, x.exits[k], em.n);
    add(&em, RCNT, RCNT, 3);
    ret = em.n;
    mr(&em, 3, RCNT);
    epilogue(&em);
    for (k = 0; k < x.nchain_ret; k++) patch(&em, x.chain_ret[k], ret);
    (void)chain;
    return em.err ? 0 : em.n;
}

/* ------------------------------------------------------------------ cache des blocs */

static void code_written(VR4300 *c, uint32_t pa)
{
    jit_invalidate(c, pa, 1);
}

void jit_flush(VR4300 *c)
{
    Jit *j = (Jit *)c->jit;
    uint32_t i;
    if (!j) return;
    for (i = 0; i < j->npages; i++) if (j->map[i]) { free(j->map[i]); j->map[i] = NULL; }
    memset(j->codepage, 0, j->npages);
    j->used = 0;
    j->flushes++;
}

void jit_invalidate(VR4300 *c, uint32_t pa, uint32_t len)
{
    Jit *j = (Jit *)c->jit;
    uint32_t p, last;
    if (!j || !len) return;
    pa &= 0x1FFFFFFF;
    last = (pa + len - 1) >> PAGE_SHIFT;
    for (p = pa >> PAGE_SHIFT; p <= last && p < j->npages; p++) {
        if (!j->codepage[p]) continue;
        j->codepage[p] = 0;
        if (j->map[p]) { free(j->map[p]); j->map[p] = NULL; }
    }
}

int jit_init(VR4300 *c, const JitBackend *b)
{
    Jit *j = (Jit *)calloc(1, sizeof *j);
    if (!j) return -1;
    j->be = *b;
    j->npages = (c->rdram_size + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
    j->map = (uint32_t **)calloc(j->npages, sizeof *j->map);
    j->codepage = (uint8_t *)calloc(j->npages, 1);
    if (!j->map || !j->codepage) { free(j->map); free(j->codepage); free(j); return -1; }
    j->h_exec1 = b->fn_addr(b->ctx, (void *)vr4300_exec1);
    j->h_load = b->fn_addr(b->ctx, (void *)vr4300_jit_load);
    j->h_store = b->fn_addr(b->ctx, (void *)vr4300_jit_store);
    c->jit = j;
    c->codepage = j->codepage;
    c->code_written = code_written;
    c->jit_map = j->map;
    c->jit_npages = j->npages;
    c->jit_code_base = b->code_base;
    return 0;
}

void jit_free(VR4300 *c)
{
    Jit *j = (Jit *)c->jit;
    if (!j) return;
    jit_flush(c);
    free(j->map); free(j->codepage); free(j);
    c->jit = NULL; c->codepage = NULL; c->code_written = NULL;
}

int jit_compile_test(VR4300 *c, uint32_t pc, uint32_t *out, int max_words)
{
    Jit *j = (Jit *)c->jit;
    int idle;
    return j ? compile_block(j, c, pc, out, max_words, &idle) : 0;
}

/* bloc pour pc (compile si besoin) ; 0 si impossible */
static uint32_t lookup(Jit *j, VR4300 *c, uint32_t pc)
{
    uint32_t pa = pc & 0x1FFFFFFF, pg = pa >> PAGE_SHIFT, slot = (pa & 0xFFF) >> 2, ent;
    int n, idle;
    if (pg >= j->npages) return 0;
    if (j->map[pg] && (ent = j->map[pg][slot]) != 0) return ent;
    n = compile_block(j, c, pc, j->tmp, TMP_WORDS, &idle);
    if (n <= 0) return 0;
    if (j->used + (uint32_t)n * 4 > j->be.code_size) {
        jit_flush(c);
        if ((uint32_t)n * 4 > j->be.code_size) return 0;
    }
    if (!j->map[pg]) {
        j->map[pg] = (uint32_t *)calloc(1024, sizeof(uint32_t));
        if (!j->map[pg]) return 0;
    }
    if (j->be.write(j->be.ctx, j->used, j->tmp, n)) return 0;
    ent = ENTRY(j->used, idle);
    j->map[pg][slot] = ent;
    j->codepage[pg] = 1;
    j->used += (uint32_t)n * 4;
    j->compiled++;
    c->jit_blocks = j->compiled;
    return ent;
}

uint64_t jit_run(VR4300 *c, uint64_t cycles)
{
    Jit *j = (Jit *)c->jit;
    uint64_t start = c->cycles, end = start + cycles;
    const unsigned cpi = c->cpi ? c->cpi : 1;
    if (!j) return vr4300_run(c, cycles);
    while (c->cycles < end) {
        uint32_t pc = c->pc, ent, n;
        if (!c->in_delay && (c->cp0[CP0_CAUSE] & 0xFF00) && vr4300_check_int(c)) { c->cycles += 2; continue; }
        if (c->in_delay || c->branch_pending || (pc & 0xC0000000u) != 0x80000000u || (pc & 3) ||
            (ent = lookup(j, c, pc)) == 0) {
            vr4300_run(c, cpi);                                  /* une instruction interpretee */
            continue;
        }
        {
            uint64_t left = (end - c->cycles + cpi - 1) / cpi;
            c->jit_budget = left > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)left;
        }
        n = j->be.call(j->be.ctx, ENTRY_OFF(ent), c);
        j->calls++;
        c->r[0] = 0;
        c->instructions += n;
        c->jit_instr += n;
        vr4300_advance(c, (uint64_t)n * cpi);
        if (ENTRY_IDLE(ent) && c->pc == pc && c->cycles < end && !(c->cp0[CP0_CAUSE] & c->cp0[CP0_STATUS] & 0xFF00)) {  /* boucle d'attente : on saute le temps */
            c->idle_skips++;
            c->idle_cycles += end - c->cycles;
            vr4300_advance(c, end - c->cycles);
        }
    }
    return c->cycles - start;
}

#ifdef JIT_SELFTEST
/* suite d'instructions de reference, comparee a l'assembleur LLVM par tools/jit_encoding_check.py */
int jit_encoder_selftest(uint32_t *out, int max)
{
    Em em = { out, 0, max, 0 };
    Em *e = &em;
    int a, b;
    addi(e, 4, 31, -8); addis(e, 5, 0, 0x1234); ori(e, 4, 5, 0xBEEF); oris(e, 6, 7, 0x8000); xori(e, 8, 9, 0xFFFF);
    andi_(e, 4, 4, 31); lwz(e, 4, 31, 320); stw(e, 4, 31, 12); ld(e, 7, 31, 248); std_(e, 7, 31, 16); stdu(e, 1, 1, -160);
    mr(e, 31, 3); extsw(e, 4, 4); extsb(e, 7, 7); add(e, 4, 4, 5); subf(e, 4, 5, 4);
    x_(e, 28, 4, 4, 5); x_(e, 444, 4, 4, 5); x_(e, 316, 4, 4, 5); x_(e, 124, 4, 4, 5);
    x_(e, 24, 4, 4, 5); x_(e, 536, 4, 4, 5); x_(e, 792, 4, 4, 5); x_(e, 27, 4, 4, 5); x_(e, 539, 4, 4, 5); x_(e, 794, 4, 4, 5);
    x_(e, 87, 7, 30, 6); x_(e, 343, 7, 30, 6); x_(e, 279, 7, 30, 6); x_(e, 23, 7, 30, 6); x_(e, 21, 7, 30, 6);
    x_(e, 215, 7, 30, 6); x_(e, 407, 7, 30, 6); x_(e, 151, 7, 30, 6); x_(e, 149, 7, 30, 6);
    rlwinm(e, 5, 4, 2, 30, 31); rlwinm(e, 6, 4, 0, 3, 31); rlwinm(e, 8, 6, 20, 12, 31); rlwinm(e, 4, 4, 9, 31, 31);
    rlwinm(e, 4, 4, 5, 0, 26); rlwinm(e, 4, 4, 27, 5, 31);
    rldicr(e, 4, 4, 5, 58); rldicr(e, 4, 4, 32, 31); rldicr(e, 4, 4, 40, 23); rldicl(e, 4, 4, 59, 5); rldicl(e, 4, 4, 20, 44);
    sradi(e, 4, 4, 3); sradi(e, 4, 4, 35); srawi(e, 4, 4, 7);
    cmpd(e, 4, 5); cmpld(e, 4, 5); cmplw(e, 6, 28); cmpdi(e, 4, -3); cmpwi(e, 5, 2);
    mtctr(e, 12); bctrl(e);
    a = bc_fwd(e, BO_TRUE, C_EQ); b = bc_fwd(e, BO_FALSE, C_LT); E(e, 0x60000000u); patch(e, a, e->n); patch(e, b, e->n);
    a = b_fwd(e); E(e, 0x60000000u); patch(e, a, e->n);
    li64(e, 12, 0x0123456789ABCDEFull);
    andis_(e, 10, 9, 0x0400); rldimi(e, 8, 7, 0, 32); rldimi(e, 8, 7, 16, 8); mulld(e, 6, 4, 5);
    rldicl(e, 4, 4, 0, 32); E(e, 0x4E800420u);
    return em.n;
}
#endif

uint64_t jit_stat_calls(VR4300 *c) { Jit *j = (Jit *)c->jit; return j ? j->calls : 0; }

void jit_info(VR4300 *c, uint32_t *used, uint32_t *size, uint64_t *flushes, uint64_t *compiled)
{
    Jit *j = (Jit *)c->jit;
    if (!j) { *used = *size = 0; *flushes = *compiled = 0; return; }
    *used = j->used; *size = j->be.code_size; *flushes = j->flushes; *compiled = j->compiled;
}
