#ifndef DADA_TOUCH_GPIO_UEFI_H
#define DADA_TOUCH_GPIO_UEFI_H
#include "efi.h"
typedef void (*dada_gpio_log_fn)(const char *,...);
int dada_gpio_uefi_probe(efi_boot_services *,dada_gpio_log_fn);
int dada_gpio_uefi_start(efi_boot_services *,dada_gpio_log_fn);
int dada_gpio_uefi_read(uint8_t *,unsigned);
int dada_gpio_uefi_write(const uint8_t *,unsigned);
int dada_gpio_uefi_stop(void);
#endif
