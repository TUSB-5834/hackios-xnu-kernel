/* Resident firmware query service. Does not map GPU registers or execute FW. */
#include "dada_gpu_service.h"
#include <sys/dada_gpu.h>
/* Only called before publishing the service; avoid a large Windows-ABI stack. */
static struct dada_gmu_plan plan;

int
dada_gpu_service_init(struct dada_gpu_shared *p, const uint8_t *firmware,
    size_t length, const char *sha256)
{
    if (!p) return DADA_GPU_BAD_ARGUMENT;
    *p = (struct dada_gpu_shared){0};
    if (!sha256) return DADA_GPU_BAD_ARGUMENT;
    /* The hash is build provenance, not a claim of runtime authentication. */
    for (unsigned i = 0; i < 64; i++) {
        if (!((sha256[i] >= '0' && sha256[i] <= '9') ||
              (sha256[i] >= 'a' && sha256[i] <= 'f')))
            return DADA_GPU_BAD_ARGUMENT;
    }
    if (sha256[64] != 0) return DADA_GPU_BAD_ARGUMENT;
    p->header.magic = DADA_GPU_SHARED_MAGIC;
    p->header.version = DADA_GPU_SHARED_VERSION;
    p->header.size = sizeof(*p);
    p->info.chipid = DADA_GPU_CHIPID;
    p->info.core_pa = DADA_GPU_CORE_PA;
    p->info.gmu_pa = DADA_GPU_GMU_PA;
    p->info.gpucc_pa = DADA_GPU_CC_PA;
    p->info.smmu_pa = DADA_GPU_SMMU_PA;
    p->info.profile_source = DADA_GPU_PROFILE_FROM_DT;
    p->info.capabilities = DADA_GPU_CAP_QUERY;
    for (unsigned i = 0; i < 65; i++) p->info.gmu_sha256[i] = sha256[i];
    int result = dada_gmu_plan_firmware(firmware, length, &plan);
    p->info.validation_result = result;
    if (result == 0) {
        p->info.gmu_bytes = (uint32_t)length;
        p->info.blocks = plan.blocks;
        p->info.metadata_blocks = plan.metadata_blocks;
        p->info.data_blocks = plan.data_blocks;
        p->info.capabilities |= DADA_GPU_CAP_GMU_VALIDATED;
    }
    return result;
}

int
dada_gpu_service_poll(struct dada_gpu_shared *p)
{
    if (!p || p->header.magic != DADA_GPU_SHARED_MAGIC ||
        p->header.version != DADA_GPU_SHARED_VERSION ||
        p->header.size != sizeof(*p)) return 0;
    uint32_t request = p->ask.request;
    if (request == 0 || request == p->answer.answered) return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    int32_t status = p->ask.operation == DADA_GPU_OP_QUERY_INFO ?
        DADA_GPU_REPLY_OK : DADA_GPU_REPLY_UNSUPPORTED;
    for (unsigned i = 0; i < 30; i++) {
        if (p->ask.reserved[i] != 0) status = DADA_GPU_REPLY_MALFORMED;
    }
    p->answer.status = status;
    p->answer.served++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->answer.answered = request;
    return 1;
}
