/* jit_backend_pc.c : "zone executable" du recompilateur pour les tests sur PC.
 * Le code PowerPC genere est execute par l'interpreteur PowerPC de test. */
#include "../source/jit.h"
#include "ppc_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_CODE_SIZE (8u << 20)
static uint32_t *code_area;

static uint32_t w_exec1(void *c, uint32_t a, uint32_t b) { (void)b; return (uint32_t)vr4300_exec1((VR4300 *)c, a); }
static uint32_t w_load(void *c, uint32_t a, uint32_t b) { return (uint32_t)vr4300_jit_load((VR4300 *)c, a, b); }
static uint32_t w_store(void *c, uint32_t a, uint32_t b) { return (uint32_t)vr4300_jit_store((VR4300 *)c, a, b); }

static int pc_write(void *ctx, uint32_t off, const uint32_t *w, int n)
{
    (void)ctx;
    memcpy((uint8_t *)code_area + off, w, (size_t)n * 4);
    return 0;
}

int jit_pc_errors;

static uint32_t pc_call(void *ctx, uint32_t off, VR4300 *c)
{
    int err;
    uint32_t n;
    (void)ctx;
    ppc_set_bigendian_range(c->rdram, c->rdram_size);
    n = ppc_call((uint64_t)(uintptr_t)((uint8_t *)code_area + off), (uint64_t)(uintptr_t)c, &err);
    if (err) { jit_pc_errors++; fprintf(stderr, "jit: erreur d'execution PowerPC (%d) bloc %08X\n", err, off); exit(3); }
    return n;
}

static uint64_t pc_fn_addr(void *ctx, void *fn)
{
    static uint64_t a_exec1, a_load, a_store;
    (void)ctx;
    if (!a_exec1) { a_exec1 = ppc_register_helper(w_exec1); a_load = ppc_register_helper(w_load); a_store = ppc_register_helper(w_store); }
    if (fn == (void *)vr4300_exec1) return a_exec1;
    if (fn == (void *)vr4300_jit_load) return a_load;
    if (fn == (void *)vr4300_jit_store) return a_store;
    fprintf(stderr, "jit: fonction d'aide inconnue\n");
    exit(3);
}

int jit_pc_attach(VR4300 *c)
{
    JitBackend b;
    if (!code_area) code_area = (uint32_t *)calloc(1, PC_CODE_SIZE);
    if (!code_area) return -1;
    memset(&b, 0, sizeof b);
    b.code_size = PC_CODE_SIZE;
    b.code_base = (uint64_t)(uintptr_t)code_area;
    b.write = pc_write;
    b.call = pc_call;
    b.fn_addr = pc_fn_addr;
    return jit_init(c, &b);
}
