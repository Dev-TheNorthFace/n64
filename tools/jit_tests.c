/* jit_tests.c : le recompilateur doit donner EXACTEMENT les memes resultats que
 * l'interpreteur. Chaque programme MIPS est execute deux fois (interpreteur /
 * code PowerPC genere, lui-meme execute par l'interpreteur PowerPC de test),
 * puis registres, HI/LO, PC, COP0 et memoire sont compares. */
#include "../source/jit.h"
#include "ppc_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int jit_pc_attach(VR4300 *c);
extern int jit_pc_errors;

#define RAM (4u << 20)
static uint8_t *ram_a, *ram_b;
static VR4300 A, B;
static int fails, passes;
#define CHECK(cond, m) do { if (cond) passes++; else { fails++; printf("ECHEC: %s (ligne %d)\n", m, __LINE__); } } while (0)

static uint64_t rd(void *x, uint32_t a, int s) { (void)x; (void)s; return a == 0x04400010u ? 0x123 : 0; }
static void wr(void *x, uint32_t a, int s, uint64_t v) { (void)x; (void)a; (void)s; (void)v; }

#define R(op, rs, rt, rd, sa, fn) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (uint32_t)(sa) << 6 | (uint32_t)(fn))
#define I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xFFFF))
#define J(op, t) ((uint32_t)(op) << 26 | (((uint32_t)(t) >> 2) & 0x3FFFFFF))

static void w32(uint8_t *m, uint32_t a, uint32_t v) { a &= 0x1FFFFFFF; m[a] = (uint8_t)(v >> 24); m[a + 1] = (uint8_t)(v >> 16); m[a + 2] = (uint8_t)(v >> 8); m[a + 3] = (uint8_t)v; }

static void setup(VR4300 *c, uint8_t *ram)
{
    void *j = c->jit;
    uint8_t *cp = c->codepage;
    void (*cw)(struct VR4300 *, uint32_t) = c->code_written;
    void *map = c->jit_map;
    uint64_t cb = c->jit_code_base;
    uint32_t np = c->jit_npages;
    memset(c, 0, sizeof *c);
    c->jit = j; c->codepage = cp; c->code_written = cw; c->jit_map = map; c->jit_code_base = cb; c->jit_npages = np;
    c->rdram = ram; c->rdram_size = RAM; c->read = rd; c->write = wr; c->ctx = c;
    c->cpi = 1;
    c->idle_skip = 1;
    vr4300_reset(c);
    c->cp0[CP0_STATUS] = 0x34000000;            /* CU1, FR, interruptions coupees */
    c->pc = 0x80001000;
}

/* charge le meme programme dans les deux machines, les fait tourner, compare */
static int lockstep;
static void run_both(const uint32_t *prog, int n, uint64_t cycles, const char *name)
{
    int i, same = 1;
    for (i = 0; i < n; i++) { w32(ram_a, 0x80001000u + (uint32_t)i * 4, prog[i]); w32(ram_b, 0x80001000u + (uint32_t)i * 4, prog[i]); }
    /* arret : boucle sur elle-meme juste apres le programme */
    w32(ram_a, 0x80001000u + (uint32_t)n * 4, J(2, 0x80001000u + (uint32_t)n * 4)); w32(ram_b, 0x80001000u + (uint32_t)n * 4, J(2, 0x80001000u + (uint32_t)n * 4));
    if (B.jit) jit_flush(&B);
    if (lockstep) {
        /* mise au point : avance par petits pas et compare apres chaque pas */
        uint64_t t;
        for (t = 0; t < cycles; t++) {
            uint32_t pcb = B.pc;
            jit_run(&B, 1);
            while (A.instructions < B.instructions) vr4300_run(&A, 1);
            for (i = 0; i < 32; i++) if (A.r[i] != B.r[i]) break;
            if (i < 32 || A.pc != B.pc) {
                printf("  divergence apres le bloc %08X : r%d interp %llX jit %llX, pc %08X/%08X, instr %llu/%llu\n", pcb, i,
                       (unsigned long long)A.r[i & 31], (unsigned long long)B.r[i & 31], A.pc, B.pc,
                       (unsigned long long)A.instructions, (unsigned long long)B.instructions);
                break;
            }
        }
    }
    vr4300_run(&A, cycles);
    jit_run(&B, cycles);
    for (i = 0; i < 32; i++) if (A.r[i] != B.r[i]) { if (same) printf("  %s : r%d interp %016llX jit %016llX\n", name, i, (unsigned long long)A.r[i], (unsigned long long)B.r[i]); same = 0; }
    if (A.hi != B.hi || A.lo != B.lo) { printf("  %s : HI/LO differents\n", name); same = 0; }
    if (A.pc != B.pc) { printf("  %s : pc interp %08X jit %08X\n", name, A.pc, B.pc); same = 0; }
    if (A.cp0[CP0_EPC] != B.cp0[CP0_EPC] || A.cp0[CP0_CAUSE] != B.cp0[CP0_CAUSE] || A.cp0[CP0_BADVADDR] != B.cp0[CP0_BADVADDR]) {
        printf("  %s : COP0 different (EPC %llX/%llX cause %llX/%llX bad %llX/%llX)\n", name, (unsigned long long)A.cp0[CP0_EPC], (unsigned long long)B.cp0[CP0_EPC],
               (unsigned long long)A.cp0[CP0_CAUSE], (unsigned long long)B.cp0[CP0_CAUSE], (unsigned long long)A.cp0[CP0_BADVADDR], (unsigned long long)B.cp0[CP0_BADVADDR]);
        same = 0;
    }
    for (i = 0; i < 32; i++) if (A.fpr[i] != B.fpr[i]) { printf("  %s : f%d different\n", name, i); same = 0; break; }
    if (memcmp(ram_a, ram_b, RAM)) { printf("  %s : memoire differente\n", name); same = 0; }
    CHECK(same, name);
}

static void fresh(void)
{
    memset(ram_a, 0, RAM); memset(ram_b, 0, RAM);
    setup(&A, ram_a); setup(&B, ram_b);
}

#define HALT_LOOP(i) J(2, 0x80001000u + (i) * 4), 0

/* ---- tests diriges ---- */
static void test_alu(void)
{
    uint32_t p[] = {
        I(15, 0, 8, 0x8765), I(13, 8, 8, 0x4321),          /* t0 = 0xFFFFFFFF87654321 */
        I(15, 0, 9, 0x7FFF), I(13, 9, 9, 0xFFF0),          /* t1 = 0x7FFFFFF0 */
        I(9, 8, 10, -5), I(25, 8, 11, -5), I(9, 9, 12, 100),  /* ADDIU DADDIU (debordement 32 bits) */
        R(0, 8, 9, 13, 0, 0x21), R(0, 8, 9, 14, 0, 0x23), R(0, 8, 9, 15, 0, 0x2D), R(0, 8, 9, 16, 0, 0x2F),
        R(0, 8, 9, 17, 0, 0x24), R(0, 8, 9, 18, 0, 0x25), R(0, 8, 9, 19, 0, 0x26), R(0, 8, 9, 20, 0, 0x27),
        R(0, 8, 9, 21, 0, 0x2A), R(0, 8, 9, 22, 0, 0x2B), R(0, 9, 8, 23, 0, 0x2A), R(0, 9, 8, 24, 0, 0x2B),
        I(10, 8, 25, -1), I(11, 8, 2, -1), I(10, 9, 3, 5), I(11, 9, 4, 0x7FFF),
        I(12, 8, 5, 0xF0F0), I(13, 8, 6, 0x0F0F), I(14, 8, 7, 0xFFFF),
        R(0, 0, 8, 26, 4, 0x00), R(0, 0, 8, 27, 4, 0x02), R(0, 0, 8, 28, 4, 0x03), R(0, 0, 8, 29, 31, 0x00),
        R(0, 0, 8, 30, 0, 0x02), R(0, 0, 9, 31, 31, 0x03)
    };
    fresh();
    run_both(p, (int)(sizeof p / 4), 2000, "ALU 32/64 bits, SLT, immediats, decalages");
}

static void test_shifts64(void)
{
    uint32_t p[] = {
        I(15, 0, 8, 0x8000), I(13, 8, 8, 0x1234), R(0, 0, 8, 8, 0, 0x3C), I(13, 8, 8, 0xABCD),   /* t0 64 bits */
        I(9, 0, 9, 13), I(9, 0, 10, 45),
        R(0, 0, 8, 11, 5, 0x38), R(0, 0, 8, 12, 5, 0x3A), R(0, 0, 8, 13, 5, 0x3B),
        R(0, 0, 8, 14, 7, 0x3C), R(0, 0, 8, 15, 7, 0x3E), R(0, 0, 8, 16, 7, 0x3F),
        R(0, 0, 8, 17, 0, 0x38), R(0, 0, 8, 18, 0, 0x3A),
        R(0, 9, 8, 19, 0, 0x04), R(0, 9, 8, 20, 0, 0x06), R(0, 9, 8, 21, 0, 0x07),
        R(0, 10, 8, 22, 0, 0x14), R(0, 10, 8, 23, 0, 0x16), R(0, 10, 8, 24, 0, 0x17),
        R(0, 8, 0, 0, 0, 0x11), R(0, 9, 0, 0, 0, 0x13), R(0, 0, 0, 25, 0, 0x10), R(0, 0, 0, 2, 0, 0x12),   /* MTHI MTLO MFHI MFLO */
        R(0, 8, 9, 0, 0, 0x18), R(0, 0, 0, 3, 0, 0x10), R(0, 0, 0, 4, 0, 0x12),                         /* MULT (interpretee) */
        R(0, 8, 9, 0, 0, 0x1B), R(0, 0, 0, 5, 0, 0x10), R(0, 0, 0, 6, 0, 0x12),                         /* DIVU */
        HALT_LOOP(30)
    };
    fresh();
    run_both(p, (int)(sizeof p / 4), 2000, "decalages 64 bits, HI/LO, MULT/DIVU via interpreteur");
}

static void test_memory(void)
{
    uint32_t p[] = {
        I(15, 0, 20, 0x8010), I(15, 0, 21, 0xA010),          /* s4 = 0x80100000 (KSEG0), s5 = 0xA0100000 (KSEG1) */
        I(15, 0, 8, 0x89AB), I(13, 8, 8, 0xCDEF),
        I(43, 20, 8, 0), I(41, 20, 8, 6), I(40, 20, 8, 9), I(63, 20, 8, 16),     /* SW SH SB SD */
        I(35, 20, 9, 0), I(39, 20, 10, 0), I(33, 20, 11, 4), I(37, 20, 12, 6),   /* LW LWU LH LHU */
        I(32, 20, 13, 9), I(36, 20, 14, 9), I(55, 20, 15, 16),                   /* LB LBU LD */
        I(35, 21, 16, 0), I(43, 21, 9, 32), I(35, 20, 17, 32),                   /* via KSEG1 */
        I(15, 0, 22, 0xA440), I(35, 22, 18, 0x10),                               /* registre materiel (chemin lent) */
        I(34, 20, 19, 1), I(38, 20, 19, 6),                                      /* LWL LWR (interpretees) */
        HALT_LOOP(22)
    };
    fresh();
    run_both(p, (int)(sizeof p / 4), 2000, "chargements/rangements rapides + KSEG1 + materiel + LWL/LWR");
}

static void test_branches(void)
{
    uint32_t p[] = {
        I(9, 0, 8, 0), I(9, 0, 9, 10),
        /* 2: boucle de 10 tours, la condition lue avant le creneau qui modifie t1 */
        I(9, 8, 8, 1), I(5, 8, 9, -2), I(9, 10, 10, 3),
        /* 5: likely non pris : creneau annule */
        I(21, 8, 9, 2), I(9, 0, 11, 99),
        I(9, 0, 12, 1),
        /* 8: JAL vers 12, retour par JR */
        J(3, 0x80001000u + 12 * 4), I(9, 0, 13, 5),
        I(9, 13, 13, 1),
        J(2, 0x80001000u + 16 * 4), 0,
        /* 12: fonction */ R(0, 31, 0, 0, 0, 0x08), I(9, 0, 14, 7),
        0, 0,
        /* 16: BGEZAL / BLTZ / BLEZ / BGTZ, JALR */
        I(1, 0, 17, 2), I(9, 0, 15, 1), I(9, 15, 15, 10), I(9, 0, 16, 42),
        I(1, 8, 0, 2), I(9, 16, 16, 1), I(9, 16, 16, 100),
        I(7, 9, 0, 2), I(9, 16, 16, 1000), I(9, 16, 16, 5000),
        I(15, 0, 24, 0x8000), I(13, 24, 24, 0x1000 + 30 * 4), R(0, 24, 0, 25, 0, 0x09), I(9, 0, 17, 3),
        /* 30 */ I(9, 0, 18, 9)
    };
    fresh();
    run_both(p, (int)(sizeof p / 4), 5000, "branchements, likely, JAL/JR/JALR, liens, creneaux");
}

static void test_exception_in_slot(void)
{
    /* LW non aligne dans un creneau de retard -> erreur d'adresse, EPC = branchement, BD = 1 */
    uint32_t p[] = {
        I(15, 0, 20, 0x8010),
        I(4, 0, 0, 3), I(35, 20, 8, 2),
        0, 0, 0
    };
    uint32_t handler[] = { J(2, 0x80000180u), 0 };           /* le vecteur boucle sur lui-meme */
    fresh();
    w32(ram_a, 0x80000180u, handler[0]); w32(ram_b, 0x80000180u, handler[0]);
    run_both(p, 6, 300, "exception dans un creneau (EPC, BD, BadVAddr)");
    CHECK(A.cp0[CP0_EPC] == 0xFFFFFFFF80001004ull && (A.cp0[CP0_CAUSE] & 0x80000000u), "exception : EPC = branchement, BD");
}

static void test_selfmod(void)
{
    /* le programme ecrit une instruction dans son propre code puis l'execute */
    uint32_t newins = I(9, 0, 9, 77);
    uint32_t p[] = {
        I(15, 0, 20, 0x8000), I(13, 20, 20, 0x1000 + 12 * 4),
        I(15, 0, 8, (int)(newins >> 16)), I(13, 8, 8, (int)(newins & 0xFFFF)),
        I(9, 0, 10, 0),
        /* 5: premier passage : execute l'ancienne instruction 12, puis la remplace */
        J(2, 0x80001000u + 12 * 4), 0,
        /* 7 */ I(43, 20, 8, 0), J(2, 0x80001000u + 12 * 4), 0,
        0, 0,
        /* 12 */ I(9, 0, 9, 11), I(9, 10, 10, 1), I(9, 0, 11, 2), I(5, 10, 11, -10), 0
    };
    fresh();
    run_both(p, (int)(sizeof p / 4), 5000, "code auto-modifie (invalidation des blocs)");
    CHECK(B.r[9] == 77, "code auto-modifie : la nouvelle instruction est executee");
}

static void test_fpu_bc1(void)
{
    uint32_t p[] = {
        I(15, 0, 8, 0x3F80), R(17, 4, 8, 0, 0, 0) | (2u << 11),                  /* MTC1 t0, f2 (1.0) */
        I(15, 0, 9, 0x4000), R(17, 4, 9, 0, 0, 0) | (4u << 11),                  /* f4 = 2.0 */
        R(17, 16, 4, 2, 0, 0x3C),                                                 /* C.LT.S f2, f4 */
        I(17, 8, 1, 2), I(9, 0, 10, 1), I(9, 0, 11, 2),                          /* BC1T */
        R(17, 16, 2, 4, 0, 0x3C),                                                 /* C.LT.S f4, f2 */
        I(17, 8, 3, 2), I(9, 0, 12, 1), I(9, 0, 13, 2),                          /* BC1TL (non pris) */
        R(17, 16, 4, 2, 6, 0x00),                                                 /* ADD.S f6, f2, f4 */
        HALT_LOOP(13)
    };
    fresh();
    run_both(p, (int)(sizeof p / 4), 2000, "FPU (interpretee) + BC1T/BC1TL traduits");
}

/* deplacements flottants (LWC1 SWC1 LDC1 SDC1 MTC1 MFC1 DMTC1 DMFC1) dans les deux modes FR,
 * avec changement de mode au milieu (le code traduit doit alors passer par l'interpreteur) */
static void test_fpmoves(uint32_t status, const char *name)
{
    uint32_t p[] = {
        I(15, 0, 20, 0x8010),                                   /* s4 = donnees */
        I(15, 0, 8, 0x1234), I(13, 8, 8, 0x5678), I(0x2B, 20, 8, 0), I(15, 0, 9, 0x9ABC), I(13, 9, 9, 0xDEF0), I(0x2B, 20, 9, 4),
        I(0x31, 20, 1, 0), I(0x31, 20, 2, 4), I(0x31, 20, 3, 4), I(0x31, 20, 4, 0),   /* LWC1 f1 f2 f3 f4 */
        I(0x35, 20, 6, 0),                                      /* LDC1 f6 */
        R(17, 0, 10, 1, 0, 0) | (1u << 11), R(17, 0, 11, 0, 0, 0) | (2u << 11),      /* MFC1 t2,f1 ; t3,f2 */
        R(17, 0, 12, 0, 0, 0) | (3u << 11), R(17, 1, 13, 0, 0, 0) | (6u << 11),      /* MFC1 t4,f3 ; DMFC1 t5,f6 */
        R(17, 4, 9, 0, 0, 0) | (7u << 11), R(17, 4, 8, 0, 0, 0) | (8u << 11),        /* MTC1 t1,f7 ; t0,f8 */
        R(17, 5, 13, 0, 0, 0) | (10u << 11),                    /* DMTC1 t5,f10 */
        I(0x39, 20, 7, 8), I(0x39, 20, 8, 12), I(0x39, 20, 1, 16), I(0x3D, 20, 10, 24), I(0x3D, 20, 7, 32),
        I(0x23, 20, 14, 8), I(0x23, 20, 15, 16), I(0x37, 20, 16, 24),
        R(16, 0, 17, 12, 0, 0), I(15, 0, 18, 0x0400), R(0, 17, 18, 17, 0, 0x26), R(16, 4, 17, 12, 0, 0),  /* bascule FR */
        I(0x31, 20, 5, 0), R(17, 0, 19, 0, 0, 0) | (5u << 11), I(0x39, 20, 3, 40), I(0x35, 20, 12, 0),
        R(17, 4, 9, 0, 0, 0) | (11u << 11), I(0x3D, 20, 11, 48),
        HALT_LOOP(37)
    };
    fresh();
    A.cp0[CP0_STATUS] = B.cp0[CP0_STATUS] = status;
    jit_flush(&B);
    run_both(p, (int)(sizeof p / 4), 5000, name);
}

/* ADD SUB DADD DSUB ADDI DADDI : resultat et exception de debordement */
static void test_trap_arith(void)
{
    uint32_t p[] = {
        I(15, 0, 8, 0x7FFF), I(13, 8, 8, 0xFFF0), I(9, 0, 9, 0x10), I(9, 0, 10, -5),
        R(0, 8, 10, 11, 0, 0x20), R(0, 9, 10, 12, 0, 0x22), I(8, 9, 13, 100), I(0x18, 10, 14, -100),
        R(0, 8, 9, 15, 0, 0x2C), R(0, 9, 8, 16, 0, 0x2E),
        R(0, 8, 9, 17, 0, 0x20),                               /* debordement -> exception */
        I(9, 0, 18, 1),
        HALT_LOOP(12)
    };
    static const uint32_t h[] = { R(16, 0, 26, 14, 0, 0), I(9, 26, 26, 4), R(16, 4, 26, 14, 0, 0), R(16, 16, 0, 0, 0, 24) };
    int i;
    fresh();
    for (i = 0; i < 4; i++) { w32(ram_a, 0x80000180u + (uint32_t)i * 4, h[i]); w32(ram_b, 0x80000180u + (uint32_t)i * 4, h[i]); }
    run_both(p, (int)(sizeof p / 4), 3000, "ADD/SUB/ADDI avec piege de debordement");
    CHECK(A.r[17] == 0 && A.r[18] == 1 && (A.cp0[CP0_CAUSE] & 0x7C) == 12 << 2, "debordement : exception, rd inchange");
}

/* ---- programmes aleatoires ---- */
static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

static void test_random_ex(int seed, int n, int exc);
static void test_random(int seed, int n) { test_random_ex(seed, n, 0); }

static void test_random_ex(int seed, int n, int exc)
{
    uint32_t p[400];
    int i, k;
    static const int alu_fn[] = { 0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x14, 0x16, 0x17, 0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2D, 0x2F, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F, 0x10, 0x12, 0x11, 0x13, 0x18, 0x19, 0x1C, 0x1D };
    static const int imm_op[] = { 0x09, 0x19, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
    static const int mem_op[] = { 0x20, 0x24, 0x21, 0x25, 0x23, 0x27, 0x37, 0x28, 0x29, 0x2B, 0x3F };
    static const int mem_sz[] = { 1, 1, 2, 2, 4, 4, 8, 1, 2, 4, 8 };
    char name[64];
    rnd_state = (uint32_t)seed;
    k = 0;
    p[k++] = I(15, 0, 20, 0x8010);                                            /* s4 = donnees */
    p[k++] = I(9, 0, 21, 0);
    p[k++] = I(15, 0, 23, 0xA440);                                            /* s7 = registres video */
    for (i = 1; i < 20; i++) if (i != 20 && i != 21) { p[k++] = I(15, 0, i, (int)(rnd() & 0xFFFF)); p[k++] = I(13, i, i, (int)(rnd() & 0xFFFF)); }
    while (k < n) {
        uint32_t t = rnd() % 100;
        int rs = (int)(rnd() % 20), rt = (int)(rnd() % 20), rd = 1 + (int)(rnd() % 19);
        if (exc && t < 4) { static const int tf[] = { 0x20, 0x22, 0x2C, 0x2E }; p[k++] = R(0, rs, rt, rd, 0, tf[rnd() % 4]); }
        else if (exc && t < 6) p[k++] = I(rnd() & 1 ? 0x08 : 0x18, rs, rd, (int)(rnd() & 0xFFFF));
        else if (t < 10) {
            static const int fo[] = { 0x31, 0x39, 0x35, 0x3D };
            int f = (int)(rnd() % 4), fr = (int)(rnd() % 32);
            int off = (int)((rnd() % 32) * 8);
            if (rnd() % 2) p[k++] = I(fo[f], 20, fr, off);
            else p[k++] = R(17, (int)(rnd() % 2) * 4 + (int)(rnd() % 2), rnd() % 2 ? rd : rt, 0, 0, 0) | ((uint32_t)fr << 11);
        }
        else if (t < 45) p[k++] = R(0, rs, rt, rd, (int)(rnd() % 32), alu_fn[rnd() % (sizeof alu_fn / sizeof alu_fn[0])]);
        else if (t < 70) p[k++] = I(imm_op[rnd() % 8], rs, rd, (int)(rnd() & 0xFFFF));
        else if (t < 88) {
            int m = (int)(rnd() % 11), off = (int)((rnd() % 64) * mem_sz[m]), base = 20;
            if (exc && rnd() % 6 == 0) off += 1 + (int)(rnd() % 3);              /* non aligne -> exception */
            if (exc && rnd() % 8 == 0) { base = 23; off &= ~7; }                 /* registre materiel (chemin lent) */
            p[k++] = I(mem_op[m], base, mem_op[m] >= 0x28 ? rt : rd, off);
        } else if (k + 3 < n) {
            static const int bop[] = { 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17 };
            p[k++] = I(bop[rnd() % 8], rs, rt, 2);                           /* saut en avant court */
            p[k++] = R(0, rs, rt, rd, 0, 0x21);                               /* creneau */
            p[k++] = I(9, rd, rd, 1);
        }
    }
    /* boucle externe : 5 tours (s5 compteur) */
    p[k++] = I(9, 21, 21, 1);
    p[k++] = I(9, 0, 22, 5);
    p[k] = I(5, 21, 22, (int16_t)(40 - (k + 1)));                             /* retour vers l'instruction 40 */
    k++;
    p[k++] = 0;
    p[k] = J(2, 0x80001000u + (uint32_t)k * 4); k++;
    p[k++] = 0;
    fresh();
    if (seed % 2) A.cp0[CP0_STATUS] = B.cp0[CP0_STATUS] = 0x30000000;   /* une moitie en mode FR = 0 */
    if (exc) {
        /* gestionnaire d'exception : reprend a EPC + 4 */
        static const uint32_t h[] = { R(16, 0, 26, 14, 0, 0), I(9, 26, 26, 4), R(16, 4, 26, 14, 0, 0), R(16, 16, 0, 0, 0, 24) };
        for (i = 0; i < 4; i++) { w32(ram_a, 0x80000180u + (uint32_t)i * 4, h[i]); w32(ram_b, 0x80000180u + (uint32_t)i * 4, h[i]); }
    }
    snprintf(name, sizeof name, "programme aleatoire %s%d (%d instructions)", exc ? "avec exceptions " : "", seed, k);
    run_both(p, k, 200000, name);
}

int main(void)
{
    int s;
    ram_a = (uint8_t *)calloc(RAM, 1); ram_b = (uint8_t *)calloc(RAM, 1);
    setup(&A, ram_a); setup(&B, ram_b);
    if (jit_pc_attach(&B)) { printf("jit_init impossible\n"); return 1; }
    test_alu();
    test_shifts64();
    test_memory();
    test_branches();
    test_exception_in_slot();
    test_selfmod();
    test_fpu_bc1();
    test_fpmoves(0x34000000, "deplacements flottants, FR = 1");
    test_fpmoves(0x30000000, "deplacements flottants, FR = 0");
    test_trap_arith();
    if (getenv("JITDBG")) { lockstep = 1; s = atoi(getenv("JITDBG")); test_random_ex(s, 150 + ((s - 1000) % 4) * 40, s >= 1000); return 0; }
    for (s = 1; s <= 200; s++) test_random(s, 120 + (s % 5) * 50);
    for (s = 1; s <= 100; s++) test_random_ex(1000 + s, 150 + (s % 4) * 40, 1);
    printf("Recompilateur : %d reussis, %d echecs (%llu blocs, %llu instructions PowerPC executees)\n", passes, fails,
           (unsigned long long)B.jit_blocks, (unsigned long long)ppc_executed);
    return fails ? 1 : 0;
}
