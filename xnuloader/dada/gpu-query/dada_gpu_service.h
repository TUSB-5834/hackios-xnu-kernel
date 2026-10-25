#ifndef DADA_GPU_SERVICE_H
#define DADA_GPU_SERVICE_H
#include <sys/dada_gpu_shared.h>
int dada_gpu_service_init(struct dada_gpu_shared *, const uint8_t *, size_t,
    const char *);
/* Caller supplies cache invalidation before poll and writeback after poll.
 * Returns one if an answer was published, zero when no request is pending.
 */
int dada_gpu_service_poll(struct dada_gpu_shared *);
#endif
