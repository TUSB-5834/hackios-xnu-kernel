/* dada/SM8750: use the hash-pinned TLMMDxe's own pin mapping and HAL.
 * No peripheral MMIO address is constructed or dereferenced here.
 * Snapshots contain the firmware's logical configuration, not raw registers.
 * See uefi/dada/touch/DIAGNOSTICS.md for the verified, no-This ABI.
 */
#include "dada_touch_gpio_uefi.h"
#include "dada_touch_gpio.h"

struct tlmm_id_protocol {
    uint64_t revision;
    void *get_handle, *get_id, *get_named_id, *get_property;
    efi_status (*configure)(uint64_t,uint32_t,uint32_t);
    void *inactive;
    efi_status (*input)(uint64_t,uint32_t,uint32_t *);
    efi_status (*output)(uint64_t,uint32_t,uint32_t);
    efi_status (*get_config)(uint64_t,uint32_t,uint32_t *);
};
_Static_assert(offsetof(struct tlmm_id_protocol,configure)==0x28,"TLMM ID configure ABI");
_Static_assert(offsetof(struct tlmm_id_protocol,input)==0x38,"TLMM ID input ABI");
_Static_assert(offsetof(struct tlmm_id_protocol,get_config)==0x48,"TLMM ID query ABI");
static struct tlmm_id_protocol *tlmm;
static struct dada_gpio_spi state;
static efi_boot_services *services;
static dada_gpio_log_fn note;
static uint8_t *image;
static uint32_t (*get_output)(uint64_t,uint32_t);
static uint32_t ids[6];
static unsigned raw_reads, preflight_ok;
static const unsigned pins[6]={40,41,42,43,161,162};

static int index_of(unsigned pin) {
    for(unsigned i=0;i<6;i++)if(pins[i]==pin)return (int)i;
    return -1;
}
static int readable_ram(uintptr_t start,uint64_t length) {
    static uint8_t map[32768];uint64_t size=sizeof(map),key=0,stride=0;uint32_t version=0;
    if(!length || start+length<start || !services->get_memory_map ||
        EFI_ERROR(services->get_memory_map(&size,map,&key,&stride,&version)) ||
        stride<sizeof(efi_memory_descriptor) || size>sizeof(map))return 0;
    for(uint64_t off=0;off+stride<=size;off+=stride) {
        const efi_memory_descriptor *d=(const void *)(map+off);
        if(d->type<1 || d->type>7 || (d->attribute&0x2000) || d->pages>UINT64_MAX/4096)continue;
        uint64_t bytes=d->pages*4096;
        if(start>=d->phys_start && start-d->phys_start<=bytes && length<=bytes-(start-d->phys_start))return 1;
    }
    return 0;
}
static int verified(void) {
    static const efi_guid loaded={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b}};
    efi_handle *handles=0;uint64_t count=0;
    if(!tlmm || tlmm->revision!=1 ||
        EFI_ERROR(services->locate_handle_buffer(ByProtocol,&loaded,0,&count,&handles)))return 0;
    image=0;
    for(uint64_t i=0;i<count;i++) {
        uint8_t *li=0;
        if(EFI_ERROR(services->handle_protocol(handles[i],&loaded,(void **)&li)) || !li)continue;
        uint64_t base=*(uint64_t *)(li+64),size=*(uint64_t *)(li+72);
        if(size>=0xb000 && (uintptr_t)tlmm==base+0xa128 &&
            (uintptr_t)tlmm->configure==base+0x177c &&
            (uintptr_t)tlmm->input==base+0x17e8 &&
            (uintptr_t)tlmm->output==base+0x181c &&
            (uintptr_t)tlmm->get_config==base+0x18b8)image=(uint8_t *)(uintptr_t)base;
    }
    services->free_pool(handles);
    if(image)get_output=(void *)(image+0x4ba8);
    return image!=0;
}
static int resolve_ids(void) {
    /* Firmware-owned RAM in the verified image. GetGpioId may allocate/change
     * mappings even during a probe; inherit enabled main-TLMM mappings only.
     */
    unsigned count=*(uint32_t *)(image+0xa310),tiles=*(uint32_t *)(image+0xa580);
    uint32_t *map=*(uint32_t **)(image+0xa308);
    if(!map || ((uintptr_t)map&3) || !count || count>256 || !tiles || tiles>4) {
        note("touch: GPIO existing mapping unavailable count=%d tiles=%d\n",(uint64_t)count,(uint64_t)tiles);
        return -1;
    }
    for(unsigned p=0;p<6;p++) {
        unsigned matches=0;
        for(unsigned i=0;i<count;i++) {
            uint32_t v=map[i];
            if((v&0x90000000u)!=0x90000000u || ((v>>26)&3) || ((v>>18)&255)!=pins[p])continue;
            unsigned tile=(v>>29)&3;
            if(tile>=tiles)return -1;
            uintptr_t hal=*(uintptr_t *)(image+0xa660+tile*0x50+0x30);
            uintptr_t expected=(uintptr_t)image;
            if(hal!=expected+0xa288 && hal!=expected+0xa2c0)return -1;
            uintptr_t *methods=(uintptr_t *)hal;
            if(methods[1]!=expected+(hal==expected+0xa288 ? 0x3f4c:0x40a4) ||
                methods[2]!=expected+0x3fb0 || methods[3]!=expected+0x3fcc ||
                methods[5]!=expected+0x4040 ||
                methods[6]!=expected+(hal==expected+0xa288 ? 0x405c:0x4120))return -1;
            ids[p]=0x40010000u|i;matches++;
        }
        note("touch: GPIO existing mapping gpio%d matches=%d id=%x\n",
            (uint64_t)pins[p],(uint64_t)matches,(uint64_t)(matches==1 ? ids[p]:0));
        if(matches!=1)return -1;
    }
    return 0;
}
static int logical_config(uint32_t value) {
    unsigned drive=(value>>7)&0xfff;
    /* Units are 0.01 mA. Reject eGPIO and unsupported fields. */
    return !(value&~0x7ffffu) && drive>=200 && drive<=1600 && !(drive%200);
}
static int save(void *ctx,unsigned pin,uint32_t *config,uint32_t *latch) {
    (void)ctx;int i=index_of(pin);if(i<0 || !image || !tlmm)return -1;
    note("touch: GPIO query gpio%d begin\n",(uint64_t)pin);
    uint32_t value=0;
    efi_status status=tlmm->get_config(0,ids[i],&value);
    if(EFI_ERROR(status) || !logical_config(value)) {
        note("touch: GPIO query gpio%d rejected status=%x config=%x\n",(uint64_t)pin,status,(uint64_t)value);
        return -1;
    }
    /* Pinned 4ba8 ignores its first argument, resolves this validated ID,
     * and calls HAL table+0x28 to read the output latch. It returns 0/1.
     * The legacy DAL table does not expose GetCurrentConfig.
     */
    if(!get_output)return -1;
    *config=value;*latch=get_output(0,ids[i]);
    note("touch: GPIO query gpio%d logical=%x out=%d\n",
        (uint64_t)pin,(uint64_t)*config,(uint64_t)*latch);
    return *latch>1 ? -1:0;
}
static int read_config(unsigned pin,uint32_t expected) {
    int i=index_of(pin);uint32_t value=0;
    if(i<0 || EFI_ERROR(tlmm->get_config(0,ids[i],&value)))return -1;
    if(value!=expected) {
        note("touch: GPIO readback gpio%d logical=%x expected=%x\n",
            (uint64_t)pin,(uint64_t)value,(uint64_t)expected);return -1;
    }
    return 0;
}
static int configure(void *ctx,unsigned pin,unsigned dir,unsigned pull,unsigned drive) {
    (void)ctx;int i=index_of(pin);
    if(i<0 || dir>1 || pull>3 || drive>7)return -1;
    uint32_t value=(dir<<4)|(pull<<5)|(((drive+1)*200)<<7);
    note("touch: GPIO configure gpio%d begin logical=%x\n",(uint64_t)pin,(uint64_t)value);
    efi_status status=tlmm->configure(0,ids[i],value);
    if(EFI_ERROR(status)) {
        note("touch: GPIO configure gpio%d status=%x\n",(uint64_t)pin,status);return -1;
    }
    return read_config(pin,value);
}
static int output(void *ctx,unsigned pin,unsigned value) {
    (void)ctx;int i=index_of(pin);if(i<0 || value>1)return -1;
    return EFI_ERROR(tlmm->output(0,ids[i],value)) ? -1:0;
}
static int input(void *ctx,unsigned pin,unsigned *value) {
    (void)ctx;int i=index_of(pin);if(i<0)return -1;
    return EFI_ERROR(tlmm->input(0,ids[i],value)) ? -1:0;
}
static int restore(void *ctx,unsigned pin,uint32_t config,uint32_t latch) {
    int i=index_of(pin);if(i<0 || !logical_config(config) || latch>1)return -1;
    /* Preload the old latch, restore logical mux/direction/pull/drive.
     * Reserved/ownership bits stay under firmware HAL control.
     */
    if(output(ctx,pin,latch) || EFI_ERROR(tlmm->configure(0,ids[i],config)))return -1;
    uint32_t actual=0,value=0;
    int okay=!save(ctx,pin,&actual,&value) && actual==config && value==latch;
    note("touch: GPIO logical restore gpio%d okay=%d\n",(uint64_t)pin,(uint64_t)okay);
    return okay ? 0:-1;
}
static void delay(void *ctx,unsigned us) {(void)ctx;services->stall(us);}
static uint64_t time_us(void *ctx) {
#ifdef DADA_GPIO_HOST_TEST
    extern uint64_t dada_gpio_host_time(void *);
    return dada_gpio_host_time(ctx);
#else
    (void)ctx;uint64_t ticks,hz;
    __asm__ volatile("mrs %0,cntvct_el0":"=r"(ticks));
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    return (ticks/hz)*1000000+(ticks%hz)*1000000/hz;
#endif
}
static int prepare(efi_boot_services *bs,dada_gpio_log_fn log) {
    static const efi_guid guid={0x1a7405fc,0x0285,0x41f0,{0xa5,0x35,0x01,0x81,0x41,0xf8,0x39,0xe7}};
    services=bs;note=log;
    efi_status status=bs->locate_protocol(&guid,0,(void **)&tlmm);
    if(EFI_ERROR(status) || !verified()) {
        note("touch: GPIO pinned TLMM ID protocol unavailable status=%x\n",status);return -1;
    }
    unsigned count=*(uint32_t *)(image+0xa310);
    uintptr_t map=*(uintptr_t *)(image+0xa308);
    if(!count || count>256 || !readable_ram(map,count*sizeof(uint32_t))) {
        note("touch: GPIO mapping is not a validated RAM allocation\n");return -1;
    }
    return resolve_ids();
}
int dada_gpio_uefi_probe(efi_boot_services *bs,dada_gpio_log_fn log) {
    if(state.changed || state.ready)return -1;
    preflight_ok=0;
    if(prepare(bs,log))return -1;
    for(unsigned i=0;i<6;i++) {
        uint32_t config=0,latch=0;unsigned level=2;
        if(save(0,pins[i],&config,&latch) || input(0,pins[i],&level) || level>1)return -1;
        note("touch: GPIO read-only gpio%d mux=%d input=%d out=%d\n",(uint64_t)pins[i],
            (uint64_t)(config&15),(uint64_t)level,(uint64_t)latch);
    }
    preflight_ok=1;
    note("touch: GPIO read-only preflight PASS; no configuration or output writes\n");
    return 0;
}
int dada_gpio_uefi_start(efi_boot_services *bs,dada_gpio_log_fn log) {
    if(!preflight_ok) {
        log("touch: GPIO start refused; first run oem touchprobe-gpio and retrieve its log\n");return -1;
    }
    /* Probe and configuration require independent command boundaries. */
    raw_reads=0;
    if(state.changed || state.ready)return -1;
    if(prepare(bs,log))return -1;
    note("touch: Pong GPIO SPI port MISO40 MOSI41 CLK42 CS43 reset161 IRQ162; inherited power\n");
    struct dada_gpio_ops ops={0,save,configure,output,input,restore,delay,time_us};
    int error=dada_gpio_spi_begin(&state,&ops);
    note("touch: GPIO SPI start error=%x changed=%x\n",(uint64_t)(uint32_t)error,(uint64_t)state.changed);
    return error;
}
int dada_gpio_uefi_read(uint8_t *data,unsigned length) {
    int error=dada_gpio_spi_transfer(&state,0,data,length);
    if(raw_reads++<8) {
        note("touch: GPIO RX error=%x bytes=%d first=",(uint64_t)(uint32_t)error,(uint64_t)length);
        if(!error)for(unsigned i=0;i<length && i<16;i++)note("%x ",(uint64_t)data[i]);
        note("\n");
    }
    return error;
}
int dada_gpio_uefi_write(const uint8_t *data,unsigned length) {
    return dada_gpio_spi_transfer(&state,data,0,length);
}
int dada_gpio_uefi_stop(void) {
    int error=dada_gpio_spi_end(&state);
    if(note)note("touch: GPIO stopped error=%x remaining=%x\n",(uint64_t)(uint32_t)error,(uint64_t)state.changed);
    return error;
}
