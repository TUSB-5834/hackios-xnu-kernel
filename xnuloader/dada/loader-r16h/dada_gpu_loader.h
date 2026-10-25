#ifndef DADA_GPU_LOADER_H
#define DADA_GPU_LOADER_H
#include "efi.h"
void dada_uefi_gpu_setup(efi_boot_services *, efi_handle, const uint8_t *, uint32_t,
    void (*)(const char *, ...));
void dada_uefi_gpu_poll(void);
uint64_t dada_uefi_gpu_shared_pa(void);
#endif
