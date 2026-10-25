/* Bounds-checked, freestanding decoder of the SM8750 GMU block file format.
 * This implementation does not link Linux code or issue hardware commands.
 */
#include <sys/dada_gpu.h>

static uint32_t
le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
        ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

int
dada_gmu_region(uint32_t address, uint32_t size)
{
    static const struct { uint32_t base, size; } regions[] = {
        { 0x00000000u, 0x4000u },
        { 0x00004000u, 0x00ffc000u },
        { 0x10004000u, 0x4000u },
        { 0x60000000u, 0x20000000u },
        { 0xc0000000u, 0x20000000u }
    };
    if (size == 0) return DADA_GMU_INVALID;
    uint64_t end = (uint64_t)address + size;
    for (unsigned i = 0; i < DADA_GMU_REGIONS; i++) {
        if (address >= regions[i].base &&
            end <= (uint64_t)regions[i].base + regions[i].size) return (int)i;
    }
    return DADA_GMU_INVALID;
}

int
dada_gmu_plan_firmware(const uint8_t *data, size_t length, struct dada_gmu_plan *p)
{
    if (p == NULL) return DADA_GPU_BAD_ARGUMENT;
    *p = (struct dada_gmu_plan){0};
    if (data == NULL || length > DADA_GPU_FW_MAX) return DADA_GPU_BAD_ARGUMENT;
    if (length == 0) return DADA_GPU_EMPTY;
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 16) return DADA_GPU_TRUNCATED;
        if (p->blocks >= DADA_GPU_MAX_BLOCKS) return DADA_GPU_TOO_MANY_BLOCKS;
        struct dada_gmu_block b = { .address = le32(data + offset),
            .size = le32(data + offset + 4), .type = le32(data + offset + 8),
            .value = le32(data + offset + 12), .region = DADA_GMU_INVALID };
        offset += 16;
        b.file_offset = (uint32_t)offset;
        if (b.size == 0) {
            if (b.type > 7) return DADA_GPU_BAD_TYPE;
            /* Qualcomm guarantees metadata precedes loadable payloads. */
            if (p->data_blocks != 0) return DADA_GPU_BAD_ORDER;
            p->metadata_blocks++;
            if (b.type == 1 || b.type == 7) {
                b.region = dada_gmu_region(b.address, b.value);
                if (b.region == DADA_GMU_INVALID) return DADA_GPU_BAD_REGION;
                p->preallocations++;
            } else if (b.type >= 2) {
                p->version[b.type] = b.value;
                p->version_mask |= 1u << b.type;
            }
        } else {
            /* type/value belong to metadata only. The supplied Gen8 ROM
             * fills both with 0xefbeadde on data blocks; gen8_gmu_load_fw
             * likewise selects loadable blocks by size, ignoring these.
             */
            if ((b.address | b.size) & 3u) return DADA_GPU_UNALIGNED;
            if (b.size > length - offset) return DADA_GPU_TRUNCATED;
            b.region = dada_gmu_region(b.address, b.size);
            if (b.region == DADA_GMU_INVALID) return DADA_GPU_BAD_REGION;
            p->bytes_by_region[b.region] += b.size;
            p->data_blocks++;
            offset += b.size;
        }
        p->block[p->blocks++] = b;
    }
    if (p->data_blocks == 0) return DADA_GPU_EMPTY;
    p->valid = 1;
    return 0;
}
