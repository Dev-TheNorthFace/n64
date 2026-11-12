/* cpu_tests.c : tests unitaires du VR4300 (PC uniquement). */
#include "../source/vr4300.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *ram;
static VR4300 c;
static int fails, passes;
#define CHECK(cond, m) do { if (cond) passes++; else { fails++; printf("ECHEC: %s (ligne %d)\n", m, __LINE__); } } while (0)

static uint64_t rd(void *x, uint32_t a, int s) { (void)x; (void)a; (void)s; return 0; }
static void wr(void *x, uint32_t a, int s, uint64_t v) { (void)x; (void)a; (void)s; (void)v; }

/* --- encodeurs --- */
#define R(op, rs, rt, rd, sa, fn) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (uint32_t)(sa) << 6 | (uint32_t)(fn))
#define I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xFFFF))
#define J(op, t) ((uint32_t)(op) << 26 | (((uint32_t)(t) >> 2) & 0x3FFFFFF))
enum { ZERO, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7, S0, S1, S2, S3, S4, S5, S6, S7, T8, T9, K0, K1, GP, SP, FP, RA };
#define NOP 0
#define ADDIU(t, s, i) I(9, s, t, i)
#define DADDIU(t, s, i) I(25, s, t, i)
#define LUI(t, i) I(15, 0, t, i)
#define ORI(t, s, i) I(13, s, t, i)
#define BEQ(s, t, o) I(4, s, t, o)
#define BNE(s, t, o) I(5, s, t, o)
#define BNEL(s, t, o) I(21, s, t, o)
#define BLEZL(s, o) I(22, s, 0, o)
#define SW(t, o, b) I(43, b, t, o)
#define LW(t, o, b) I(35, b, t, o)
#define SH(t, o, b) I(41, b, t, o)
#define LHU(t, o, b) I(37, b, t, o)
#define LB(t, o, b) I(32, b, t, o)
#define LWL(t, o, b) I(34, b, t, o)
#define LWR(t, o, b) I(38, b, t, o)
#define SWL(t, o, b) I(42, b, t, o)
#define SWR(t, o, b) I(46, b, t, o)
#define LDL(t, o, b) I(26, b, t, o)
#define LDR(t, o, b) I(27, b, t, o)
#define SD(t, o, b) I(63, b, t, o)
#define LD(t, o, b) I(55, b, t, o)
#define MTC0(t, d) R(16, 4, t, d, 0, 0)
#define MFC0(t, d) R(16, 0, t, d, 0, 0)
#define ERET R(16, 16, 0, 0, 0, 24)
#define MTC1(t, f) R(17, 4, t, f, 0, 0)
#define MFC1(t, f) R(17, 0, t, f, 0, 0)
#define DMFC1(t, f) R(17, 1, t, f, 0, 0)
#define CTC1(t, f) R(17, 6, t, f, 0, 0)
#define FOP(fmt, ft, fs, fd, fn) R(17, fmt, ft, fs, fd, fn)
#define BC1T(o) I(17, 8, 1, o)

static void load_prog(const uint32_t *p, int n)
{
    int i;
    memset(ram, 0, 8 << 20);
    for (i = 0; i < n; i++) {
        uint32_t w = p[i];
        uint8_t *d = ram + 0x1000 + i * 4;
        d[0] = (uint8_t)(w >> 24); d[1] = (uint8_t)(w >> 16); d[2] = (uint8_t)(w >> 8); d[3] = (uint8_t)w;
    }
    vr4300_reset(&c);
    c.rdram = ram; c.rdram_size = 8 << 20; c.read = rd; c.write = wr;
    c.pc = 0x80001000u;
    c.cp0[12] = 0x24000000;          /* FR=1, CU1 */
}
static void run(int n) { while (n--) vr4300_run(&c, 1); }
static uint32_t ram32(uint32_t a) { return (uint32_t)ram[a] << 24 | (uint32_t)ram[a + 1] << 16 | (uint32_t)ram[a + 2] << 8 | ram[a + 3]; }

int main(void)
{
    ram = calloc(8 << 20, 1);

    /* ---- etape 5 : saut des boucles d'attente */
    {   /* attente d'un mot memoire : lw / beq / nop -> sautee */
        uint32_t p[] = { LUI(T0, 0x8010), LW(T1, 0, T0), BEQ(T1, ZERO, -2), NOP, ADDIU(T2, ZERO, 7) };
        load_prog(p, 5); c.idle_skip = 1; c.idle_skips = 0; c.instructions = 0; c.cycles = 0;
        vr4300_run(&c, 10000);
        CHECK(c.idle_skips >= 1 && c.instructions < 50 && c.cycles >= 10000, "boucle d'attente sautee");
        CHECK(c.pc == 0x80001004u && c.r[T2] == 0, "boucle d'attente : etat inchange");
        CHECK((uint32_t)c.cp0[CP0_COUNT] >= 4900, "boucle d'attente : Count avance");
        ram[0x100003] = 1;                                  /* l'evenement arrive */
        vr4300_run(&c, 100);
        CHECK(c.r[T2] == 7, "boucle d'attente : sortie quand la memoire change");
    }
    {   /* compteur : addiu / bne -> PAS une attente */
        uint32_t p[] = { ADDIU(T0, ZERO, 0), ADDIU(T1, ZERO, 3000), ADDIU(T0, T0, 1), BNE(T0, T1, -2), NOP, ADDIU(T2, ZERO, 7) };
        load_prog(p, 6); c.idle_skip = 1; c.idle_skips = 0;
        vr4300_run(&c, 20000);
        CHECK(c.idle_skips == 0 && c.r[T2] == 7 && c.r[T0] == 3000, "boucle de comptage executee normalement");
    }
    {   /* boucle infinie "j self" */
        uint32_t p[] = { J(2, 0x80001000), NOP };
        load_prog(p, 2); c.idle_skip = 1; c.cp0[CP0_COMPARE] = 300;
        vr4300_run(&c, 5000);
        CHECK(c.idle_skips >= 1 && (c.cp0[CP0_CAUSE] & 0x8000), "j self sautee + interruption Compare signalee");
    }
    {   /* 2 cycles par instruction */
        uint32_t p[] = { NOP, NOP, NOP, NOP };
        load_prog(p, 4); c.cpi = 2; c.instructions = 0; c.idle_skip = 0;
        vr4300_run(&c, 8);
        CHECK(c.instructions == 4 && (uint32_t)c.cp0[CP0_COUNT] == 4, "2 cycles par instruction");
        c.cpi = 1;
    }

    { uint32_t p[] = { ADDIU(T0, ZERO, -1), LUI(T1, 0x8000), ORI(T1, T1, 0x1234) };
      load_prog(p, 3); run(3);
      CHECK(c.r[T0] == 0xFFFFFFFFFFFFFFFFull, "ADDIU -1 etendu en 64 bits");
      CHECK(c.r[T1] == 0xFFFFFFFF80001234ull, "LUI/ORI etendu en signe"); }

    { uint32_t p[] = { ADDIU(T0, ZERO, 1), R(0, 0, T0, T1, 4, 0x3C), R(0, 0, T1, T2, 0, 0x3B), R(0, 0, T0, T3, 31, 0) };
      load_prog(p, 4); run(4);
      CHECK(c.r[T1] == 0x0000001000000000ull, "DSLL32");
      CHECK(c.r[T3] == 0xFFFFFFFF80000000ull, "SLL 31 etendu en signe"); }

    /* creneau de retard : BEQ pris, l'instruction suivante s'execute */
    { uint32_t p[] = { BEQ(ZERO, ZERO, 2), ADDIU(T0, ZERO, 5), ADDIU(T1, ZERO, 7), ADDIU(T2, ZERO, 9) };
      load_prog(p, 4); run(3);
      CHECK(c.r[T0] == 5 && c.r[T1] == 0 && c.r[T2] == 9, "BEQ + creneau de retard"); }

    /* likely non pris : creneau annule */
    { uint32_t p[] = { BNEL(ZERO, ZERO, 4), ADDIU(T0, ZERO, 5), ADDIU(T1, ZERO, 7) };
      load_prog(p, 3); run(2);
      CHECK(c.r[T0] == 0 && c.r[T1] == 7, "BNEL non pris annule le creneau"); }
    { uint32_t p[] = { BLEZL(ZERO, 2), ADDIU(T0, ZERO, 5), ADDIU(T1, ZERO, 7), ADDIU(T2, ZERO, 9) };
      load_prog(p, 4); run(3);
      CHECK(c.r[T0] == 5 && c.r[T1] == 0 && c.r[T2] == 9, "BLEZL pris execute le creneau"); }

    /* JAL / JR */
    { uint32_t p[] = { J(3, 0x80001010), NOP, ADDIU(T0, ZERO, 1), NOP, R(0, RA, 0, 0, 0, 8), ADDIU(T1, ZERO, 2) };
      load_prog(p, 6); run(4);
      CHECK(c.r[RA] == 0xFFFFFFFF80001008ull && c.r[T1] == 2 && c.r[T0] == 0 && c.pc == 0x80001008u, "JAL / JR (retour apres les creneaux)"); }

    /* memoire */
    { uint32_t p[] = { LUI(T0, 0x8010), ADDIU(T1, ZERO, -2), SW(T1, 0, T0), LB(T2, 3, T0), LHU(T3, 2, T0), SH(ZERO, 0, T0), LW(T4, 0, T0) };
      load_prog(p, 7); run(7);
      CHECK(c.r[T2] == 0xFFFFFFFFFFFFFFFEull && c.r[T3] == 0xFFFE && c.r[T4] == 0xFFFE, "SW/LB/LHU/SH/LW (big-endian)"); }

    /* chargement non aligne LWL/LWR */
    { uint32_t p[] = { LUI(T0, 0x8010), LWL(T1, 1, T0), LWR(T1, 4, T0) };
      load_prog(p, 3);
      ram[0x100000] = 0x11; ram[0x100001] = 0x22; ram[0x100002] = 0x33; ram[0x100003] = 0x44; ram[0x100004] = 0x55;
      run(3);
      CHECK(c.r[T1] == 0x22334455, "LWL/LWR : mot non aligne"); }
    { uint32_t p[] = { LUI(T0, 0x8010), LUI(T1, 0xAABB), ORI(T1, T1, 0xCCDD), SWL(T1, 1, T0), SWR(T1, 4, T0) };
      load_prog(p, 5); run(5);
      CHECK(ram32(0x100000) == 0x00AABBCC && ram[0x100004] == 0xDD, "SWL/SWR : ecriture non alignee"); }
    { uint32_t p[] = { LUI(T0, 0x8010), LDL(T1, 3, T0), LDR(T1, 10, T0) };
      load_prog(p, 3);
      { int i; for (i = 0; i < 16; i++) ram[0x100000 + i] = (uint8_t)(0x10 + i); }
      run(3);
      CHECK(c.r[T1] == 0x131415161718191Aull, "LDL/LDR : double mot non aligne"); }

    /* multiplication / division */
    { uint32_t p[] = { ADDIU(T0, ZERO, -3), ADDIU(T1, ZERO, 7), R(0, T0, T1, 0, 0, 0x18), R(0, 0, 0, T2, 0, 0x12), R(0, 0, 0, T3, 0, 0x10) };
      load_prog(p, 5); run(5);
      CHECK(c.r[T2] == (uint64_t)-21 && c.r[T3] == (uint64_t)-1, "MULT -3*7"); }
    { uint32_t p[] = { ADDIU(T0, ZERO, 7), R(0, T0, ZERO, 0, 0, 0x1A), R(0, 0, 0, T2, 0, 0x12), R(0, 0, 0, T3, 0, 0x10) };
      load_prog(p, 4); run(4);
      CHECK(c.r[T2] == (uint64_t)-1 && c.r[T3] == 7, "DIV par zero (comportement N64)"); }
    { uint32_t p[] = { ADDIU(T0, ZERO, -1), ADDIU(T1, ZERO, 2), R(0, T0, T1, 0, 0, 0x1D), R(0, 0, 0, T2, 0, 0x12), R(0, 0, 0, T3, 0, 0x10) };
      load_prog(p, 5); run(5);
      CHECK(c.r[T2] == 0xFFFFFFFFFFFFFFFEull && c.r[T3] == 1, "DMULTU 128 bits"); }
    { uint32_t p[] = { ADDIU(T0, ZERO, -100), ADDIU(T1, ZERO, 7), R(0, T0, T1, 0, 0, 0x1E), R(0, 0, 0, T2, 0, 0x12), R(0, 0, 0, T3, 0, 0x10) };
      load_prog(p, 5); run(5);
      CHECK(c.r[T2] == (uint64_t)-14 && c.r[T3] == (uint64_t)-2, "DDIV signe"); }
    { uint32_t p[] = { LUI(T0, 0x7FFF), ORI(T0, T0, 0xFFFF), I(8, T0, T1, 1) };
      load_prog(p, 3); run(3);
      CHECK(c.last_exception == 12 && c.pc == 0x80000180u, "ADDI : exception de depassement"); }

    /* FPU */
    { uint32_t p[] = { LUI(T0, 0x3FC0), MTC1(T0, 1), LUI(T0, 0x4020), MTC1(T0, 2),   /* 1.5f, 2.5f */
                       FOP(16, 2, 1, 3, 0), MFC1(T1, 3),                               /* ADD.S -> 4.0 */
                       FOP(16, 0, 3, 4, 13), MFC1(T2, 4),                              /* TRUNC.W.S */
                       FOP(16, 2, 1, 0, 0x3C), BC1T(2), ADDIU(T3, ZERO, 1), ADDIU(T4, ZERO, 1), ADDIU(T5, ZERO, 1) }; /* C.LT.S */
      load_prog(p, 13); run(12);
      CHECK(c.r[T1] == 0x40800000, "ADD.S 1.5 + 2.5 = 4.0");
      CHECK(c.r[T2] == 4, "TRUNC.W.S");
      CHECK(c.r[T3] == 1 && c.r[T4] == 0 && c.r[T5] == 1, "C.LT.S + BC1T"); }
    { uint32_t p[] = { LUI(T0, 0xC020), MTC1(T0, 1), FOP(16, 0, 1, 2, 13), MFC1(T1, 2), FOP(16, 0, 1, 3, 15), MFC1(T2, 3),  /* -2.5 */
                       FOP(16, 0, 1, 4, 12), MFC1(T3, 4) };
      load_prog(p, 8); run(8);
      CHECK((int32_t)c.r[T1] == -2 && (int32_t)c.r[T2] == -3 && (int32_t)c.r[T3] == -2, "TRUNC/FLOOR/ROUND de -2.5"); }
    { uint32_t p[] = { ADDIU(T0, ZERO, 9), MTC1(T0, 1), FOP(20, 0, 1, 2, 0x21), FOP(17, 0, 2, 3, 4), FOP(17, 0, 3, 4, 0x20), MFC1(T1, 4) };
      load_prog(p, 6); run(6);
      CHECK(c.r[T1] == 0x40400000, "CVT.D.W / SQRT.D / CVT.S.D : sqrt(9) = 3"); }
    { uint32_t p[] = { ADDIU(T0, ZERO, 1), CTC1(T0, 31), LUI(T0, 0x3FF0), MTC1(T0, 1), FOP(16, 0, 1, 2, 0x24), MFC1(T1, 2) };   /* arrondi vers 0 */
      load_prog(p, 6); run(6);
      CHECK(c.r[T1] == 1, "CVT.W.S avec mode d'arrondi FCSR"); }

    /* interruption du minuteur (Count/Compare) puis ERET */
    { uint32_t p[] = { ADDIU(T0, ZERO, 3), MTC0(T0, 11), MTC0(ZERO, 9), LUI(T1, 0x2400), ORI(T1, T1, 0x8001), MTC0(T1, 12),
                       NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP };
      load_prog(p, 16);
      { uint32_t h[] = { MFC0(K0, 13), MTC0(ZERO, 11), ERET }; int i; for (i = 0; i < 3; i++) { uint8_t *d = ram + 0x180 + i * 4; d[0] = (uint8_t)(h[i] >> 24); d[1] = (uint8_t)(h[i] >> 16); d[2] = (uint8_t)(h[i] >> 8); d[3] = (uint8_t)h[i]; } }
      run(20);
      CHECK((c.r[K0] & 0x8000) && (c.r[K0] & 0x7C) == 0, "interruption du minuteur (IP7)");
      CHECK(c.pc >= 0x80001018u && c.pc < 0x80001060u && !(c.cp0[12] & 2), "ERET : retour au programme"); }

    /* TLB : virtuel 0x00400000 -> physique 0x00200000 */
    { uint32_t p[] = { LUI(T0, 0x0040), MTC0(T0, 10),                      /* EntryHi VPN2 */
                       LUI(T1, 0), ORI(T1, T1, (0x200000 >> 12 << 6) | 0x07), MTC0(T1, 2),   /* EntryLo0 : PFN, D, V, G */
                       ORI(T1, ZERO, 1), MTC0(T1, 3),                     /* EntryLo1 : G */
                       MTC0(ZERO, 5), MTC0(ZERO, 0), R(16, 16, 0, 0, 0, 2), /* PageMask, Index, TLBWI */
                       LW(T2, 0x10, T0) };
      load_prog(p, (int)(sizeof p / sizeof p[0]));
      ram[0x200010] = 0xCA; ram[0x200011] = 0xFE; ram[0x200012] = 0xBA; ram[0x200013] = 0xBE;
      run((int)(sizeof p / sizeof p[0]));
      CHECK((uint32_t)c.r[T2] == 0xCAFEBABE, "TLB : traduction d'adresse"); }
    { uint32_t p[] = { LUI(T0, 0x0070), LW(T1, 0, T0) };
      load_prog(p, 2); run(2);
      CHECK(c.pc == 0x80000000u && c.last_exception == 2 && (uint32_t)c.cp0[8] == 0x00700000u, "TLB : defaut -> vecteur de remplissage"); }
    { uint32_t p[] = { LUI(T0, 0x8010), LW(T1, 2, T0) };
      load_prog(p, 2); run(2);
      CHECK(c.last_exception == 4 && c.pc == 0x80000180u, "LW non aligne -> erreur d'adresse"); }

    printf("%d tests reussis, %d echecs\n", passes, fails);
    free(ram);
    return fails != 0;
}
