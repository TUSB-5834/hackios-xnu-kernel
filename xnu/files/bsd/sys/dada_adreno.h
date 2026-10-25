/* Gen8 backend contract for the existing RedwoodGPU msm-compatible service. */
#ifndef _SYS_DADA_ADRENO_H_
#define _SYS_DADA_ADRENO_H_
#include <stdint.h>
#include <stdbool.h>
void dada_adreno_init(void);
void dada_adreno_quiesce(void);
void dada_adreno_stop(void);
void dada_adreno_minute(void);
bool dada_gpu_ready(void);
bool dada_gpu_parameter(uint32_t, uint64_t *);
uint64_t dada_gpu_address(uint64_t);
void dada_gpu_address_free(uint64_t, uint64_t);
void dada_gpu_space(uint64_t *, uint32_t *, uint64_t *, unsigned *, unsigned *, uint64_t *, uint32_t *);
bool dada_gpu_map(uint64_t, uint64_t, bool);
void dada_gpu_mapped(void);
void dada_gpu_unmap(uint64_t, uint64_t);
void dada_gpu_to_memory(void *, uint64_t);
bool dada_gpu_done(uint32_t);
uint32_t dada_gpu_last(void);
void dada_gpu_worked(uint64_t *, uint32_t *, uint32_t[7]);
uint32_t dada_gpu_submit(const uint64_t *, const uint32_t *, uint32_t);
void dada_gpu_report(const char *);
bool dada_gpu_recover(void);
void dada_gpu_stop(void);
#endif
