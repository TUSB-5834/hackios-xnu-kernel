/* v16 resident UEFI/XNU GPU mailbox, version 1. Query only, no GPU DMA.
 * Separate 128-byte ownership regions prevent cache writeback from replacing
 * another CPU's request/answer. The containing allocation is a private 16K page.
 */
#ifndef DADA_GPU_SHARED_H
#define DADA_GPU_SHARED_H
#include <stdint.h>
#include <stddef.h>

#define DADA_GPU_SHARED_MAGIC UINT64_C(0x3155504741444144)
#define DADA_GPU_SHARED_VERSION 1u
#define DADA_GPU_SHARED_PAGE 0x4000u
#define DADA_GPU_CAP_QUERY 1u
#define DADA_GPU_CAP_GMU_VALIDATED 2u
/* Reserved capability; the current service must never advertise this bit. */
#define DADA_GPU_CAP_SUBMIT 4u
#define DADA_GPU_PROFILE_FROM_DT 1u
#define DADA_GPU_OP_QUERY_INFO 1u
#define DADA_GPU_REPLY_OK 0
#define DADA_GPU_REPLY_UNSUPPORTED (-1)
#define DADA_GPU_REPLY_MALFORMED (-2)

struct dada_gpu_info {
    uint32_t chipid, core_pa, gmu_pa, gpucc_pa, smmu_pa;
    uint32_t gmu_bytes, blocks, metadata_blocks, data_blocks;
    int32_t validation_result;
    uint32_t capabilities, profile_source;
    char gmu_sha256[65];
    uint8_t reserved[15];
};

struct dada_gpu_shared {
    struct {
        uint64_t magic;
        uint32_t version, size;
        uint8_t reserved[112];
    } header;
    struct dada_gpu_info info; /* UEFI initializes once; XNU reads only. */
    struct { /* XNU writes; UEFI invalidates before reading. */
        volatile uint32_t request;
        uint32_t operation, reserved[30];
    } ask;
    struct { /* UEFI writes and cleans; XNU maps uncached. */
        volatile uint32_t answered;
        int32_t status;
        uint64_t served;
        uint32_t reserved[28];
    } answer;
};

_Static_assert(sizeof(struct dada_gpu_info) == 128, "GPU info ABI");
_Static_assert(offsetof(struct dada_gpu_shared, info) == 128, "GPU info offset");
_Static_assert(offsetof(struct dada_gpu_shared, ask) == 256, "GPU request ownership");
_Static_assert(offsetof(struct dada_gpu_shared, answer) == 384, "GPU answer ownership");
_Static_assert(sizeof(struct dada_gpu_shared) == 512, "GPU shared ABI");
#endif
