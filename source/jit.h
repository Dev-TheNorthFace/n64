/* jit.h : recompilateur dynamique MIPS (VR4300) -> PowerPC 64 bits (PPU du Cell).
 *
 * Les blocs d'instructions MIPS sont traduits une fois en code PowerPC, puis
 * executes directement. Les registres MIPS restent dans la structure VR4300
 * (aucune allocation de registres) : simple et sur. Les instructions rares
 * (COP0, FPU, multiplications, divisions...) appellent l'interpreteur pour
 * cette seule instruction. Les acces memoire ont un chemin rapide en ligne
 * (KSEG0/KSEG1 en RDRAM) et un chemin lent par l'interpreteur.
 *
 * Le code genere est du PowerPC 64 bits big-endian, ABI ELFv1 (celle de la
 * PS3). Sur PC, un interpreteur PowerPC de test (tools/ppc_interp.c) l'execute
 * pour verifier la traduction. */
#ifndef N64_JIT_H
#define N64_JIT_H

#include <stdint.h>
#include "vr4300.h"

typedef struct {
    uint32_t code_size;                          /* taille de la zone executable (octets) */
    uint64_t code_base;                          /* adresse de la zone (pour l'enchainement des blocs) */
    /* copie n mots a l'offset off de la zone executable ; 0 si OK */
    int (*write)(void *ctx, uint32_t off, const uint32_t *words, int n);
    /* execute le bloc a l'offset off ; renvoie le nombre d'instructions MIPS faites */
    uint32_t (*call)(void *ctx, uint32_t off, VR4300 *c);
    /* adresse a charger dans CTR pour appeler une fonction C depuis le code genere */
    uint64_t (*fn_addr)(void *ctx, void *fn);
    void *ctx;
} JitBackend;

int  jit_init(VR4300 *c, const JitBackend *b);   /* 0 si OK */
void jit_info(VR4300 *c, uint32_t *used, uint32_t *size, uint64_t *flushes, uint64_t *compiled);
void jit_free(VR4300 *c);
uint64_t jit_run(VR4300 *c, uint64_t cycles);    /* comme vr4300_run */
void jit_invalidate(VR4300 *c, uint32_t pa, uint32_t len);
void jit_flush(VR4300 *c);

/* encodeur expose pour les tests */
int  jit_compile_test(VR4300 *c, uint32_t pc, uint32_t *out, int max_words);

#endif
