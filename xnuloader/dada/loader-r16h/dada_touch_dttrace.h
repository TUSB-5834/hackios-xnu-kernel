#ifndef DADA_TOUCH_DTTRACE_H
#define DADA_TOUCH_DTTRACE_H
#include <stdint.h>
#include <stddef.h>
typedef void (*dada_dttrace_log_fn)(const char *,...);
/* Only the pinned SPI consumer's private cached interface pointer is swapped.
 * The registered firmware protocol, methods and original DT stay untouched.
 */
int dada_dttrace_begin(uintptr_t *,const void *,dada_dttrace_log_fn);
int dada_dttrace_end(void);
#endif
