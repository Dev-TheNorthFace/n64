/* vr4300.h : processeur NEC VR4300 (MIPS III, 64 bits) de la Nintendo 64.
 * Interpreteur : jeu d'instructions entier complet, creneaux de retard,
 * branchements "likely", COP0 (exceptions, interruptions, Count/Compare),
 * TLB 32 entrees, FPU COP1 (simple/double, mot/long, comparaisons, arrondis). */
#ifndef N64_VR4300_H
#define N64_VR4300_H

#include <stdint.h>

/* registres COP0 */
enum {
    CP0_INDEX = 0, CP0_RANDOM, CP0_ENTRYLO0, CP0_ENTRYLO1, CP0_CONTEXT, CP0_PAGEMASK, CP0_WIRED,
    CP0_BADVADDR = 8, CP0_COUNT, CP0_ENTRYHI, CP0_COMPARE, CP0_STATUS, CP0_CAUSE, CP0_EPC, CP0_PRID,
    CP0_CONFIG, CP0_LLADDR, CP0_WATCHLO, CP0_WATCHHI, CP0_XCONTEXT,
    CP0_TAGLO = 28, CP0_TAGHI, CP0_ERROREPC
};

/* codes d'exception */
enum { EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5,
       EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15 };

typedef struct {
    uint32_t mask;          /* PageMask */
    uint64_t hi;            /* EntryHi (VPN2 | ASID) */
    uint64_t lo[2];         /* EntryLo0/1 */
    int global;
} TlbEntry;

typedef struct VR4300 {
    uint64_t r[32];
    uint64_t hi, lo;
    uint32_t pc, next_pc, delay_target;
    int in_delay, branch_pending;
    uint64_t cp0[32];
    int llbit;
    uint64_t fpr[32];
    uint32_t fcr31;
    TlbEntry tlb[32];
    uint64_t cycles;
    unsigned count_half;
    uint64_t instructions;
    uint32_t last_exception;
    uint64_t exceptions;

    /* vitesse (etape 5) */
    unsigned cpi;               /* cycles par instruction (1 = exact, 2 = defaut des emulateurs) */
    int idle_skip;              /* saut des boucles d'attente */
    uint64_t idle_skips, idle_cycles;
    struct { uint32_t pc, sig; int8_t verdict; } idle_cache[64];

    /* recompilateur (etape 6) */
    void *jit;                  /* etat du recompilateur (NULL = interpreteur seul) */
    uint8_t *codepage;          /* 1 octet par page de 4 Ko de RDRAM : du code y a ete compile */
    void (*code_written)(struct VR4300 *c, uint32_t pa);   /* une page de code a ete modifiee */
    uint64_t jit_blocks, jit_instr;
    void *jit_map;              /* table des blocs (lue par le code genere pour enchainer les blocs) */
    uint64_t jit_code_base;     /* adresse de la zone de code */
    uint32_t jit_budget;        /* instructions MIPS encore permises dans cet appel */
    uint32_t jit_npages;

    /* memoire : acces rapide a la RDRAM (octets en big-endian) */
    uint8_t *rdram;
    uint32_t rdram_size;
    void *ctx;
    uint64_t (*read)(void *ctx, uint32_t paddr, int size);
    void     (*write)(void *ctx, uint32_t paddr, int size, uint64_t v);
} VR4300;

void vr4300_reset(VR4300 *c);

/* services pour le recompilateur (jit.c) */
int  vr4300_exec1(VR4300 *c, uint32_t ins);                 /* 1 instruction interpretee ; 1 si exception */
int  vr4300_jit_load(VR4300 *c, uint32_t va, uint32_t kind);  /* kind = rt | taille << 8 | signe << 12 */
int  vr4300_jit_store(VR4300 *c, uint32_t va, uint32_t kind);
int  vr4300_check_int(VR4300 *c);
void vr4300_advance(VR4300 *c, uint64_t cycles);
int  vr4300_idle_loop(VR4300 *c, uint32_t target, uint32_t branch_pc);
/* execute environ 'cycles' cycles ; renvoie le nombre de cycles faits */
uint64_t vr4300_run(VR4300 *c, uint64_t cycles);
void vr4300_set_ip(VR4300 *c, int line, int on);    /* lignes IP2..IP7 */

/* acces virtuels (utilises aussi par les tests) ; renvoient 0 si OK */
int vr4300_virt_to_phys(VR4300 *c, uint32_t vaddr, int write, uint32_t *paddr);

#endif
