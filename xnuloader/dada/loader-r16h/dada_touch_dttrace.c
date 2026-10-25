/* Observe the exact DT API calls made by the ABI-pinned dada SPIDxe.
 * Offsets and call signatures are checked against SPIDxe.efi thunks at
 * 0x23ac..0x2448. Wrappers forward each call once and preserve its result.
 * No MMIO, controller commands, global protocol patching or DT writes here.
 */
#include "dada_touch_dttrace.h"
typedef int32_t (*path_fn)(void *,const char *);
typedef int32_t (*scalar_fn)(void *,const char *,void *);
typedef int32_t (*string_fn)(void *,const char *,char *,uint32_t);
static uintptr_t original[29],proxy[29],*consumer,previous;
static dada_dttrace_log_fn note;
static unsigned calls;
static int32_t path(void *node,const char *name) {
    note("touch: DT[%d] path %s begin\n",(uint64_t)++calls,name);
    int32_t r=((path_fn)original[6])(node,name);
    note("touch: DT path result=%x\n",(uint64_t)(uint32_t)r);return r;
}
static int32_t scalar(unsigned index,unsigned width,void *node,const char *name,void *output) {
    note("touch: DT[%d] property %s width=%d begin\n",(uint64_t)++calls,name,(uint64_t)width);
    int32_t r=((scalar_fn)original[index])(node,name,output);
    uint64_t value=0;
    if(!r && output)for(unsigned i=0;i<width;i++)value|=(uint64_t)((uint8_t *)output)[i]<<(8*i);
    note("touch: DT property %s result=%x value=%x\n",name,(uint64_t)(uint32_t)r,value);return r;
}
static int32_t size(void *node,const char *name,void *out) {return scalar(18,4,node,name,out);}
static int32_t u32(void *node,const char *name,void *out) {return scalar(20,4,node,name,out);}
static int32_t u8(void *node,const char *name,void *out) {return scalar(25,1,node,name,out);}
static int32_t u16(void *node,const char *name,void *out) {return scalar(28,2,node,name,out);}
static int32_t string(void *node,const char *name,char *out,uint32_t bytes) {
    note("touch: DT[%d] string %s capacity=%d begin\n",(uint64_t)++calls,name,(uint64_t)bytes);
    int32_t r=((string_fn)original[19])(node,name,out,bytes);
    char value[49]={0};
    if(!r && out)for(unsigned i=0;i<bytes && i<sizeof(value)-1;i++) {
        uint8_t c=(uint8_t)out[i];if(!c)break;value[i]=c>=32 && c<127 ? (char)c:'.';
    }
    note("touch: DT string %s result=%x value=%s\n",name,(uint64_t)(uint32_t)r,value);return r;
}
int dada_dttrace_begin(uintptr_t *slot,const void *registered,dada_dttrace_log_fn log) {
    if(consumer || !slot || !registered || !log || *slot!=(uintptr_t)registered)return -1;
    const uintptr_t *table=registered;
    static const unsigned used[]={6,18,19,20,25,28};
    for(unsigned i=0;i<sizeof(used)/sizeof(used[0]);i++)if(!table[used[i]])return -1;
    for(unsigned i=0;i<29;i++)original[i]=proxy[i]=table[i];
    proxy[6]=(uintptr_t)path;proxy[18]=(uintptr_t)size;proxy[19]=(uintptr_t)string;
    proxy[20]=(uintptr_t)u32;proxy[25]=(uintptr_t)u8;proxy[28]=(uintptr_t)u16;
    note=log;calls=0;previous=*slot;consumer=slot;*slot=(uintptr_t)proxy;
    note("touch: DT trace attached to pinned SPI consumer\n");return 0;
}
int dada_dttrace_end(void) {
    if(!consumer)return -1;
    /* The only mutation is ours; always restore the exact previous pointer. */
    int changed=*consumer!=(uintptr_t)proxy;
    *consumer=previous;consumer=0;
    note("touch: DT trace restored calls=%d unexpected_slot_change=%d\n",(uint64_t)calls,(uint64_t)changed);
    return changed ? -1:0;
}
