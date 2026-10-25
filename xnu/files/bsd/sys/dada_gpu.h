/* dada Adreno 830 Gen8 bring-up. No hardware access in this interface.
 * Board addresses: supplied dada stock-dada.dts. GMU format: Qualcomm's
 * SM8750 Gen8 driver; provenance in docs/dada/gpu-provenance.json.
 */
#ifndef _SYS_DADA_GPU_H_
#define _SYS_DADA_GPU_H_
#include <stdint.h>
#include <stddef.h>

#define DADA_GPU_CHIPID 0x44050001u
#define DADA_GPU_CORE_PA 0x03d00000u
#define DADA_GPU_CORE_SIZE 0x40000u
#define DADA_GPU_GMU_PA 0x03d37000u
#define DADA_GPU_GMU_SIZE 0x68000u
#define DADA_GPU_GMU_AO_PA 0x03d40000u
#define DADA_GPU_RSCC_PA 0x03d50000u
#define DADA_GPU_CC_PA 0x03d90000u
#define DADA_GPU_CC_SIZE 0x9800u
#define DADA_GPU_GX_CLKCTL_PA 0x03d68024u
#define DADA_GPU_SMMU_PA 0x03da0000u
#define DADA_GPU_SMMU_SIZE 0x40000u
#define DADA_GPU_MAX_BLOCKS 256u
#define DADA_GPU_FW_MAX (4u * 1024u * 1024u)

enum dada_gmu_region {
    DADA_GMU_ITCM, DADA_GMU_CACHE, DADA_GMU_DTCM,
    DADA_GMU_KERNEL, DADA_GMU_KERNEL_EXT, DADA_GMU_REGIONS,
    DADA_GMU_INVALID = -1
};
enum dada_gpu_error {
    DADA_GPU_BAD_ARGUMENT = -1, DADA_GPU_TRUNCATED = -2,
    DADA_GPU_BAD_REGION = -3, DADA_GPU_BAD_TYPE = -4,
    DADA_GPU_TOO_MANY_BLOCKS = -5, DADA_GPU_EMPTY = -6,
    DADA_GPU_BAD_ORDER = -7, DADA_GPU_UNALIGNED = -8
};
struct dada_gmu_block {
    uint32_t address, size, type, value;
    uint32_t file_offset; /* payload, or offset after metadata header */
    int region;
};
struct dada_gmu_plan {
    unsigned valid, blocks, data_blocks, metadata_blocks, preallocations, version_mask;
    uint32_t version[8]; /* firmware-provided metadata values, not an ABI claim */
    uint32_t bytes_by_region[DADA_GMU_REGIONS];
    struct dada_gmu_block block[DADA_GPU_MAX_BLOCKS];
};

int dada_gmu_region(uint32_t, uint32_t);
/* A successful plan describes bytes only: it does not load or execute firmware.
 * Any error leaves valid=0. Callers must not consume a partial plan.
 */
int dada_gmu_plan_firmware(const uint8_t *, size_t, struct dada_gmu_plan *);
#endif
