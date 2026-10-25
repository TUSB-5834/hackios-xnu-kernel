/* Select only one firmware-owned-free interval; never join reserved gaps. */
#ifndef DADA_MEMORY_WINDOW_H
#define DADA_MEMORY_WINDOW_H
#include "efi.h"

#define DADA_MEMORY_LIMIT 0x200000000ULL /* 8 GiB; bounded by bootstrap tables */
#define DADA_MEMORY_ALIGN 0x02000000ULL

static void dada_memory_candidate(uint64_t low, uint64_t high, uint64_t forced,
    uint64_t *base, uint64_t *bytes)
{
    if (low > UINT64_MAX - (DADA_MEMORY_ALIGN - 1)) return;
    low = (low + DADA_MEMORY_ALIGN - 1) & ~(DADA_MEMORY_ALIGN - 1);
    high &= ~(DADA_MEMORY_ALIGN - 1);
    if (forced) {
        if ((forced & (DADA_MEMORY_ALIGN - 1)) || forced < low || forced >= high) return;
        low = forced;
    }
    if (high <= low) return;
    uint64_t size = high - low;
    if (size > DADA_MEMORY_LIMIT) size = DADA_MEMORY_LIMIT;
    if (size > *bytes) { *base = low; *bytes = size; }
}

static int dada_memory_select(const void *map, uint64_t map_bytes, uint64_t stride,
    uint64_t payload_low, uint64_t payload_high, uint64_t forced,
    uint64_t *base, uint64_t *bytes)
{
    *base = *bytes = 0;
    if (stride < sizeof(efi_memory_descriptor) || map_bytes % stride ||
        payload_high <= payload_low) return 0;
    for (uint64_t offset = 0; offset < map_bytes; offset += stride) {
        const efi_memory_descriptor *d = (const void *)((const uint8_t *)map + offset);
        if (d->type != EfiConventionalMemory || d->pages > (UINT64_MAX - d->phys_start) / 4096) continue;
        uint64_t low = d->phys_start, high = low + d->pages * 4096;
        if (low < payload_high && high > payload_low) {
            if (low < payload_low) dada_memory_candidate(low, payload_low, forced, base, bytes);
            if (high > payload_high) dada_memory_candidate(payload_high, high, forced, base, bytes);
        } else dada_memory_candidate(low, high, forced, base, bytes);
    }
    return *bytes != 0;
}
#endif
