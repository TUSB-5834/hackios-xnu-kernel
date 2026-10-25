#ifndef DADA_TOUCH_CLOCK_H
#define DADA_TOUCH_CLOCK_H
#include "efi.h"
/* Stable prefix of Qualcomm EFIClock.h; no register access in this adapter. */
struct dada_clock_protocol {
    uint64_t version;
    efi_status (*get_id)(struct dada_clock_protocol *,const char *,uint64_t *);
    efi_status (*enable)(struct dada_clock_protocol *,uint64_t);
    efi_status (*disable)(struct dada_clock_protocol *,uint64_t);
    efi_status (*is_enabled)(struct dada_clock_protocol *,uint64_t,uint8_t *);
};
typedef void (*dada_clock_log_fn)(const char *,...);
struct dada_touch_clocks {
    struct dada_clock_protocol *protocol;
    uint64_t ids[5];
    unsigned held;
};
int dada_touch_clocks_start(struct dada_touch_clocks *,struct dada_clock_protocol *,dada_clock_log_fn);
void dada_touch_clocks_stop(struct dada_touch_clocks *,dada_clock_log_fn);
#endif
