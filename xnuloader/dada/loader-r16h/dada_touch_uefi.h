/* SynapticsTCMDxe dada SPI port, resident with UEFI on the observer CPU. */
#ifndef DADA_TOUCH_UEFI_H
#define DADA_TOUCH_UEFI_H
#include "efi.h"
#include <sys/dada_touch_shared.h>
typedef void (*dada_touch_log_fn)(const char *, ...);
typedef void (*dada_touch_cache_fn)(uint64_t, uint64_t);
void dada_uefi_touch_setup(efi_boot_services *, efi_handle, const void *, uint32_t,
    dada_touch_log_fn, dada_touch_cache_fn, dada_touch_cache_fn);
void dada_uefi_touch_start(void);
void dada_uefi_touch_start_gpio(void);
void dada_uefi_touch_probe_gpio(void);
void dada_uefi_touch_poll(void);
void dada_uefi_touch_stop(void);
uint32_t dada_uefi_touch_state(void);
uint64_t dada_uefi_touch_shared_pa(void);
const void *dada_uefi_touch_dt(uint32_t *);
#endif
