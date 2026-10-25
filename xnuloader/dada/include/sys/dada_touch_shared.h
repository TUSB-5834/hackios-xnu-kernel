/* UEFI producer / XNU consumer. Each writer owns separate cache lines. */
#ifndef _SYS_DADA_TOUCH_SHARED_H_
#define _SYS_DADA_TOUCH_SHARED_H_
#include <stdint.h>
#include <stddef.h>
#define DADA_TOUCH_MAGIC UINT64_C(0x3148435441444144) /* DADATCH1 */
#define DADA_TOUCH_VERSION 1
#define DADA_TOUCH_RING 64
enum { DADA_TOUCH_OFF, DADA_TOUCH_DISCOVERED, DADA_TOUCH_READY,
    DADA_TOUCH_FAILED, DADA_TOUCH_STOPPED };
struct dada_touch_event { uint32_t type, id, x, y; };
struct dada_touch_shared {
    uint64_t magic;
    uint32_t version, size, state, error, frames, dropped;
    uint32_t producer, reserved[7];
    /* Only XNU writes this cache line. */
    uint32_t consumer, stop, reserved_consumer[14];
    struct dada_touch_event events[DADA_TOUCH_RING];
};
_Static_assert(offsetof(struct dada_touch_shared, consumer) == 64, "consumer cache line");
_Static_assert(offsetof(struct dada_touch_shared, events) == 128, "event offset");
_Static_assert(sizeof(struct dada_touch_shared) == 1152, "shared ABI");
#endif
