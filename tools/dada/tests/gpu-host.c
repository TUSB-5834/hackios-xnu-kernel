/* Test pure parsers and VA accounting. Never accesses a phone or GPU MMIO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dada_gpu.h>
#include <sys/dada_gpu_va.h>
static struct dada_gmu_plan plan;
static uint64_t used, largest, lost;
static uint32_t live, refused;
static unsigned holes, peak;
static void stats(void) { dada_va_stats(&used, &live, &largest, &holes, &peak, &lost, &refused); }
static void put32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; i++) p[i] = n >> (8 * i);
}
static void header(unsigned char *p, uint32_t addr, uint32_t size, uint32_t type, uint32_t value)
{
    put32(p, addr); put32(p+4, size); put32(p+8, type); put32(p+12, value);
}
static void firmware_cases(void)
{
    unsigned char data[48] = {0};
    header(data, 0x4000, 0, 1, 0x1000);
    header(data+16, 0x4000, 4, 0xefbeadde, 0xefbeadde);
    assert(dada_gmu_plan_firmware(data, 36, &plan) == 0);
    assert(plan.valid && plan.preallocations == 1 && plan.data_blocks == 1);
    for (unsigned i = 0; i < 36; i++) {
        assert(dada_gmu_plan_firmware(data, i, &plan) != 0);
        assert(!plan.valid);
    }
    assert(dada_gmu_plan_firmware(NULL, 36, &plan) == DADA_GPU_BAD_ARGUMENT);
    assert(dada_gmu_plan_firmware(data, DADA_GPU_FW_MAX+1, &plan) == DADA_GPU_BAD_ARGUMENT);
    header(data+16, 0x3ffc, 8, 1, 0);
    assert(dada_gmu_plan_firmware(data, 40, &plan) == DADA_GPU_BAD_REGION);
    header(data+16, 0x4001, 4, 1, 0);
    assert(dada_gmu_plan_firmware(data, 36, &plan) == DADA_GPU_UNALIGNED);
    header(data, 0, 0, 8, 0);
    assert(dada_gmu_plan_firmware(data, 16, &plan) == DADA_GPU_BAD_TYPE);
    header(data, 0x4000, 4, 3, 0);
    header(data+20, 0x5000, 0, 1, 0x1000);
    assert(dada_gmu_plan_firmware(data, 36, &plan) == DADA_GPU_BAD_ORDER);
    unsigned char many[16 * (DADA_GPU_MAX_BLOCKS+1)] = {0};
    assert(dada_gmu_plan_firmware(many, sizeof(many), &plan) == DADA_GPU_TOO_MANY_BLOCKS);
    assert(dada_gmu_region(0xfffffffcu, 8) == DADA_GMU_INVALID);
    puts("GMU malformed/truncated/boundary cases: PASS");
}
static void allocator_cases(void)
{
    dada_va_init(0);
    assert(!dada_va_alloc(0) && !dada_va_alloc(UINT64_MAX));
    uint64_t a=dada_va_alloc(1), b=dada_va_alloc(65537), c=dada_va_alloc(65536);
    assert(a == DADA_VA_START && b == a+65536 && c == b+131072);
    assert(dada_va_contains(b+4, 131068) && !dada_va_contains(b+4, 131069));
    stats(); assert(live == 3 && used == 262144 && refused == 2);
    dada_va_free(b, 1); stats(); assert(live == 3); /* wrong-size free */
    dada_va_free(b, 65537); dada_va_free(b, 65537); stats(); assert(live == 2);
    dada_va_free(a,1); dada_va_free(c,65536);
    stats(); assert(!live && !used && holes == 1 && largest == DADA_VA_SIZE && !lost);
    a=dada_va_alloc(DADA_VA_SIZE); assert(a && !dada_va_alloc(1));
    dada_va_free(a,DADA_VA_SIZE);
    dada_va_init(2); a=dada_va_alloc(1); b=dada_va_alloc(1);
    assert(b == a+131072 && !dada_va_contains(a+65536,1));
    dada_va_free(a,1); dada_va_free(b,1); stats(); assert(largest == DADA_VA_SIZE && !used);
    /* Random allocation/free order must conserve all addresses. */
    uint64_t addr[512]={0}, size[512]={0}; unsigned seed=0x8750;
    for (unsigned i=0; i<20000; i++) {
        seed=seed*1664525u+1013904223u; unsigned slot=(seed>>8)%512;
        if (addr[slot]) { dada_va_free(addr[slot],size[slot]); addr[slot]=0; }
        else { size[slot]=(seed%32+1)*4096; addr[slot]=dada_va_alloc(size[slot]); assert(addr[slot]); }
    }
    for (unsigned i=0;i<512;i++) if(addr[i]) dada_va_free(addr[i],size[i]);
    stats(); assert(!used && !live && !lost && largest == DADA_VA_SIZE && holes == 1);
    puts("VA overflow/guards/coalescing/randomized accounting: PASS");
}
int main(int argc,char **argv)
{
    firmware_cases(); allocator_cases();
    if(argc>1) {
        FILE *f=fopen(argv[1],"rb"); assert(f);
        assert(!fseek(f,0,SEEK_END)); long n=ftell(f); assert(n>0 && n<=DADA_GPU_FW_MAX);
        rewind(f); unsigned char *data=malloc(n); assert(data);
        assert(fread(data,1,n,f)==(size_t)n); fclose(f);
        assert(!dada_gmu_plan_firmware(data,n,&plan));
        printf("Actual GMU firmware: PASS, %u blocks, %u payloads, %u preallocations\n",
               plan.blocks,plan.data_blocks,plan.preallocations);
        free(data);
    }
    return 0;
}
