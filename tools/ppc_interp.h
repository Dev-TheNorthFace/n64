/* ppc_interp.h : interpreteur PowerPC 64 de test (voir ppc_interp.c). */
#ifndef PPC_INTERP_H
#define PPC_INTERP_H
#include <stdint.h>

typedef uint32_t (*PpcHelper)(void *c, uint32_t a, uint32_t b);
uint64_t ppc_register_helper(PpcHelper f);              /* adresse factice a appeler */
void ppc_set_bigendian_range(uint8_t *lo, uint32_t size);
uint32_t ppc_call(uint64_t entry, uint64_t arg0, int *error);
extern uint64_t ppc_executed;

#endif
