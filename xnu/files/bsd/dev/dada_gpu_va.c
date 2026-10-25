/* Bounds-checked first-fit allocator, independent of XNU and hardware. */
#include <sys/dada_gpu_va.h>
#define GRANULE 0x10000ull
#define HOLES 1024
#define LIVE 4096
struct run { uint64_t start, size; };
static struct run holes[HOLES], live[LIVE];
static unsigned hole_count, live_count, peak, guard;
static uint64_t used, lost;
static uint32_t refused;

static uint64_t rounded(uint64_t size)
{
    if (!size || size > DADA_VA_SIZE) return 0;
    return (size + GRANULE - 1) & ~(GRANULE - 1);
}

void dada_va_init(unsigned mode)
{
    hole_count = 1; live_count = 0; peak = 1;
    guard = mode >= 2; used = lost = 0; refused = 0;
    holes[0] = (struct run){DADA_VA_START, DADA_VA_SIZE};
}

uint64_t dada_va_alloc(uint64_t size)
{
    uint64_t span = rounded(size);
    if (!span || live_count == LIVE) { refused++; return 0; }
    uint64_t reserved = span + (guard ? GRANULE : 0);
    for (unsigned i = 0; i < hole_count; i++) {
        if (holes[i].size < reserved) continue;
        uint64_t address = holes[i].start;
        holes[i].start += reserved; holes[i].size -= reserved;
        if (!holes[i].size) {
            for (unsigned j = i + 1; j < hole_count; j++) holes[j - 1] = holes[j];
            hole_count--;
        }
        live[live_count++] = (struct run){address, span};
        used += reserved;
        return address;
    }
    refused++;
    return 0;
}

bool dada_va_contains(uint64_t address, uint64_t size)
{
    if (!size || address < DADA_VA_START || address >= DADA_VA_START + DADA_VA_SIZE ||
        size > DADA_VA_START + DADA_VA_SIZE - address) return false;
    for (unsigned i = 0; i < live_count; i++) {
        if (address >= live[i].start && address - live[i].start < live[i].size &&
            size <= live[i].size - (address - live[i].start)) return true;
    }
    return false;
}

void dada_va_free(uint64_t address, uint64_t size)
{
    uint64_t span = rounded(size);
    unsigned record;
    for (record = 0; record < live_count; record++)
        if (live[record].start == address && live[record].size == span) break;
    if (!span || record == live_count) return; /* Reject duplicate or partial frees. */
    live[record] = live[--live_count];
    span += guard ? GRANULE : 0;
    used -= span;
    unsigned at = 0;
    while (at < hole_count && holes[at].start < address) at++;
    if (at && holes[at - 1].start + holes[at - 1].size == address) {
        holes[at - 1].size += span;
        if (at < hole_count && address + span == holes[at].start) {
            holes[at - 1].size += holes[at].size;
            for (unsigned j = at + 1; j < hole_count; j++) holes[j - 1] = holes[j];
            hole_count--;
        }
    } else if (at < hole_count && address + span == holes[at].start) {
        holes[at].start = address; holes[at].size += span;
    } else if (hole_count < HOLES) {
        for (unsigned j = hole_count; j > at; j--) holes[j] = holes[j - 1];
        holes[at] = (struct run){address, span}; hole_count++;
    } else lost += span;
    if (hole_count > peak) peak = hole_count;
}

void dada_va_stats(uint64_t *bytes, uint32_t *buffers, uint64_t *largest,
    unsigned *count, unsigned *most, uint64_t *leaked, uint32_t *failures)
{
    *largest = 0;
    for (unsigned i = 0; i < hole_count; i++)
        if (holes[i].size > *largest) *largest = holes[i].size;
    *bytes = used; *buffers = live_count; *count = hole_count;
    *most = peak; *leaked = lost; *failures = refused;
}
