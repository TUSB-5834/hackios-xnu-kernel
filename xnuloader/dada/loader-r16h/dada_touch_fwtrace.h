/* Trace only the pinned SPI image's private SerialPort Lib consumer. */
#ifndef DADA_TOUCH_FWTRACE_H
#define DADA_TOUCH_FWTRACE_H
#include <stdint.h>
typedef void (*dada_fwtrace_log_fn)(const char *,...);
int dada_fwtrace_begin(uintptr_t *consumer,dada_fwtrace_log_fn log);
int dada_fwtrace_end(void);
#endif
