#ifndef DADA_TOUCH_CONFIG_H
#define DADA_TOUCH_CONFIG_H
#include <stdint.h>
#include <stddef.h>
struct dada_touch_config {
    void *dt;
    uint32_t bytes, instance, enabled, pins_valid, status_bytes;
    uint8_t *status, *active, *sleep;
};
int dada_touch_config_parse(void *, size_t, struct dada_touch_config *);
int dada_touch_config_prepare(struct dada_touch_config *);
#endif
