/* Private UEFI protocol. Never expose these function pointers to XNU. */
#ifndef DADA_GPU_PROTOCOL_H
#define DADA_GPU_PROTOCOL_H
#include "efi.h"
#include <sys/dada_gpu_shared.h>
#define DADA_GPU_PROTOCOL_GUID { 0x93b4f61e, 0x65a0, 0x48f3, \
    { 0x87, 0x71, 0x1b, 0xca, 0x6f, 0x20, 0xa4, 0xd8 } }
struct dada_gpu_protocol {
    uint32_t version, size;
    efi_status (EFIAPI *get_shared)(struct dada_gpu_protocol *, uint64_t *, uint32_t *);
    efi_status (EFIAPI *poll)(struct dada_gpu_protocol *);
};
#endif
