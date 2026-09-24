/* Zbb siphash variant: same code as siphash.c, compiled with
 * -march=rv64gc_zba_zbb (see Makefile). Selected at runtime. */
#define SIPHASH_RISCV_ZBB
#include "siphash.c"
