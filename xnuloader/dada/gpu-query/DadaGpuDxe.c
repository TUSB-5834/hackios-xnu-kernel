/* Standalone AArch64 UEFI boot-service driver for the resident v16 loader.
 * Stage 1: Gen8 firmware validation + a real cross-CPU query mailbox.
 * No GPU register, clock, power, SMMU or command submission access exists here.
 */
#include "dada_gpu_protocol.h"
#include "dada_gpu_service.h"
#include <sys/dada_gpu_firmware.h>

#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED (EFI_ERROR_BIT | 3)
#define EFI_ALREADY_STARTED (EFI_ERROR_BIT | 20)
#define EFI_NATIVE_INTERFACE 0
#define EFI_BOOT_SERVICES_DATA 4
#define ALLOCATION_PAGES 8u

static struct dada_gpu_shared *shared;
static const efi_guid protocol_guid = DADA_GPU_PROTOCOL_GUID;

void *memset(void *dst, int value, size_t count)
{
    uint8_t *p = dst;
    while (count--) *p++ = (uint8_t)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (count--) *d++ = *s++;
    return dst;
}

static void cache_range(void *address, size_t bytes, int invalidate)
{
    uint64_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
    uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15);
    uintptr_t first = (uintptr_t)address & ~(line - 1);
    uintptr_t end = (uintptr_t)address + bytes;
    for (uintptr_t at = first; at < end; at += line) {
        if (invalidate) __asm__ volatile ("dc ivac, %0" :: "r" (at) : "memory");
        else __asm__ volatile ("dc cvac, %0" :: "r" (at) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
}

static efi_status EFIAPI get_shared(struct dada_gpu_protocol *self,
    uint64_t *physical, uint32_t *bytes)
{
    (void)self;
    if (!physical || !bytes || !shared) return EFI_INVALID_PARAMETER;
    *physical = (uint64_t)(uintptr_t)shared;
    *bytes = DADA_GPU_SHARED_PAGE;
    return 0;
}

static efi_status EFIAPI poll(struct dada_gpu_protocol *self)
{
    (void)self;
    if (!shared) return EFI_INVALID_PARAMETER;
    cache_range(&shared->ask, sizeof(shared->ask), 1);
    if (dada_gpu_service_poll(shared))
        cache_range(&shared->answer, sizeof(shared->answer), 0);
    return 0;
}

static struct dada_gpu_protocol protocol = {
    DADA_GPU_SHARED_VERSION, sizeof(struct dada_gpu_protocol), get_shared, poll
};

efi_status EFIAPI efi_main(efi_handle image, efi_system_table *st)
{
    if (!st || !st->bs) return EFI_INVALID_PARAMETER;
    uint64_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
    if ((UINT64_C(4) << ((ctr >> 16) & 15)) > 128) return EFI_UNSUPPORTED;
    efi_boot_services *bs = st->bs;
    void *existing = NULL;
    if (!EFI_ERROR(bs->locate_protocol(&protocol_guid, NULL, &existing)))
        return EFI_ALREADY_STARTED;
    uint64_t allocation = 0;
    efi_status status = bs->allocate_pages(AllocateAnyPages, EFI_BOOT_SERVICES_DATA,
        ALLOCATION_PAGES, &allocation);
    if (EFI_ERROR(status)) return status;
    uintptr_t aligned = ((uintptr_t)allocation + DADA_GPU_SHARED_PAGE - 1) &
        ~(uintptr_t)(DADA_GPU_SHARED_PAGE - 1);
    shared = (struct dada_gpu_shared *)aligned;
    memset(shared, 0, DADA_GPU_SHARED_PAGE);
    /* Invalid firmware stays queryable, with GMU_VALIDATED absent. */
    dada_gpu_service_init(shared, dada_gpu_gmu_firmware,
        sizeof(dada_gpu_gmu_firmware), DADA_GPU_GMU_SHA256);
    cache_range(shared, DADA_GPU_SHARED_PAGE, 0);
    status = bs->install_protocol_interface(&image, &protocol_guid,
        EFI_NATIVE_INTERFACE, &protocol);
    if (EFI_ERROR(status)) {
        shared = NULL;
        bs->free_pages(allocation, ALLOCATION_PAGES);
    }
    /* Returning success keeps this boot-service driver's code/data resident. */
    return status;
}
