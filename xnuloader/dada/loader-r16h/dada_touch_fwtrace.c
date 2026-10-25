/*
 * SPIDxe SHA256 7a70c89627891b638f1521036d73b6bd9bf927c9347d85f5d5e78dbbf83ee7c0.
 * Its private SerialPort Lib cache is at d2f0. The write thunk at 49e0..4a28
 * calls offset 10 with (buffer, byte_count), without a This argument.
 * EFI_SERIAL_PORT_LIBRARY in Mu's PlatformPeiLib/ ShimLibraries/EFISerialPort.h
 * defines eight 64-bit slots (version/padding and seven methods).
 * Copy the table, forward every write unchanged, and restore the private
 * cache after SPI open. Do not replace the globally supplied library, alter
 * its debug mask, read MMIO, or bypass any controller/interface check.
 */
#include "dada_touch_fwtrace.h"
typedef uint64_t (*write_fn)(uint8_t *,uint64_t);
static uintptr_t original[8],proxy[8],*consumer,previous;
static dada_fwtrace_log_fn note;
static uint64_t calls,bytes,omitted;
static unsigned inside;

static uint64_t write_bytes(uint8_t *buffer,uint64_t count) {
    write_fn forward=(write_fn)original[2];
    /* A console used by the logger may itself emit serial output. */
    if(inside)return forward(buffer,count);
    inside=1;calls++;bytes+=count;
    if(buffer && count && calls<=32) {
        char line[513];unsigned n=count<512 ? (unsigned)count:512;
        for(unsigned i=0;i<n;i++) {
            uint8_t c=buffer[i];
            line[i]=(c=='\n' || c=='\r' || c=='\t' || (c>=32 && c<127)) ? (char)c:'.';
        }
        line[n]=0;
        note("touch: SPI firmware: %s%s",line,n && line[n-1]=='\n' ? "":"\n");
        omitted+=count-n;
    } else omitted+=count;
    uint64_t result=forward(buffer,count);
    inside=0;return result;
}
int dada_fwtrace_begin(uintptr_t *slot,dada_fwtrace_log_fn log) {
    if(consumer || !slot || !*slot || (*slot&7) || !log)return -1;
    const uintptr_t *table=(const uintptr_t *)*slot;
    if((uint32_t)table[0]!=0x10001 || !table[2])return -1;
    for(unsigned i=0;i<8;i++)original[i]=proxy[i]=table[i];
    previous=*slot;consumer=slot;note=log;calls=bytes=omitted=0;inside=0;
    proxy[2]=(uintptr_t)write_bytes;*slot=(uintptr_t)proxy;
    return 0;
}
int dada_fwtrace_end(void) {
    if(!consumer)return -1;
    int changed=*consumer!=(uintptr_t)proxy;
    *consumer=previous;consumer=0;
    note("touch: SPI firmware trace restored calls=%d bytes=%d omitted=%d unexpected_slot_change=%d\n",
        calls,bytes,omitted,(uint64_t)changed);
    return changed ? -1:0;
}
