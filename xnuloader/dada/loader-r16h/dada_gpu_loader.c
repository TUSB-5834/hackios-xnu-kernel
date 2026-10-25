#include "dada_gpu_loader.h"
#include "dada_gpu_protocol.h"

static struct dada_gpu_protocol *gpu;
static uint64_t shared_pa;

void dada_uefi_gpu_setup(efi_boot_services *bs, efi_handle parent,
    const uint8_t *image, uint32_t bytes, void (*note)(const char *, ...))
{
    static const efi_guid guid = DADA_GPU_PROTOCOL_GUID;
    efi_handle child = NULL;
    if (gpu) return;
    if (!image || bytes < 512 || bytes > 1024 * 1024) {
        note("dada-gpu-uefi: driver not present; GPU unchanged\n");
        return;
    }
    efi_status status = bs->load_image(0, parent, NULL, (void *)image, bytes, &child);
    if (!EFI_ERROR(status)) status = bs->start_image(child, NULL, NULL);
    if (EFI_ERROR(status)) {
        note("dada-gpu-uefi: driver start failed %x; GPU unchanged\n", status);
        return;
    }
    struct dada_gpu_protocol *candidate = NULL;
    status = bs->locate_protocol(&guid, NULL, (void **)&candidate);
    if (EFI_ERROR(status) || !candidate || candidate->version != DADA_GPU_SHARED_VERSION ||
        candidate->size != sizeof(*candidate) || !candidate->get_shared || !candidate->poll) {
        note("dada-gpu-uefi: protocol unavailable or incompatible\n");
        return;
    }
    uint64_t physical = 0;
    uint32_t length = 0;
    status = candidate->get_shared(candidate, &physical, &length);
    if (EFI_ERROR(status) || !physical || (physical & (DADA_GPU_SHARED_PAGE - 1)) ||
        length != DADA_GPU_SHARED_PAGE) {
        note("dada-gpu-uefi: invalid shared page\n");
        return;
    }
    const struct dada_gpu_shared *page = (const struct dada_gpu_shared *)(uintptr_t)physical;
    if (page->header.magic != DADA_GPU_SHARED_MAGIC ||
        page->header.version != DADA_GPU_SHARED_VERSION || page->header.size != sizeof(*page) ||
        page->info.capabilities != (DADA_GPU_CAP_QUERY | DADA_GPU_CAP_GMU_VALIDATED)) {
        note("dada-gpu-uefi: query page or GMU validation failed\n");
        return;
    }
    shared_pa = physical;
    gpu = candidate;
    note("dada-gpu-uefi: r13 resident query service page=%x, hardware acceleration unavailable\n", physical);
    note("dada-gpu-uefi: GMU validated bytes=%d blocks=%d (no firmware execution)\n",
        (uint64_t)page->info.gmu_bytes, (uint64_t)page->info.blocks);
}

void dada_uefi_gpu_poll(void)
{
    if (gpu) gpu->poll(gpu);
}

uint64_t dada_uefi_gpu_shared_pa(void)
{
    return shared_pa;
}
