/* GPU virtual addresses. The RedwoodGPU caller serializes all access. */
#ifndef _SYS_DADA_GPU_VA_H_
#define _SYS_DADA_GPU_VA_H_
#include <stdint.h>
#include <stdbool.h>
#define DADA_VA_START 0x10000000ull
#define DADA_VA_SIZE 0xc0000000ull
void dada_va_init(unsigned);
uint64_t dada_va_alloc(uint64_t);
void dada_va_free(uint64_t, uint64_t);
bool dada_va_contains(uint64_t, uint64_t);
void dada_va_stats(uint64_t *, uint32_t *, uint64_t *, unsigned *, unsigned *, uint64_t *, uint32_t *);
#endif
