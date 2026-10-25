/*
 * dada SPI/AbsolutePointer adaptation of edk2-porting/edk2-msm's
 * SynapticsTCMDxe (e1952621f419f8db60ed28271264e1b5184c571d).
 * See uefi/dada/touch/UPSTREAM.md for scope, attribution and differences.
 * The bounded TouchCom core is shared with XNU's tested decoder.
 */
#include "dada_touch_uefi.h"
#include "dada_touch_config.h"
#include "dada_touch_clock.h"
#include "dada_touch_dttrace.h"
#include "dada_touch_fwtrace.h"
#include "dada_touch_gpio_uefi.h"
#include <sys/dada_touch.h>
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
static efi_boot_services *bs;
static dada_touch_log_fn note;
static dada_touch_cache_fn clean, invalidate;
static struct dada_touch_shared shared __attribute__((aligned(0x2000)));
static struct dada_touch_config config;
static struct dada_tcm_state tcm;
static void *spi_handle;
static uint8_t *spi_image;
static struct dada_touch_clocks clocks;
static unsigned active, polling, errors, pointer_changed;
static unsigned gpio_preferred,gpio_open;
static uint64_t last_poll;
static struct spi_protocol {
    uint64_t revision;
    uint32_t (*open)(uint32_t,void **);
    uint32_t (*transfer)(void *,const uint32_t *,const uint8_t *,uint32_t,uint8_t *,uint32_t);
    uint32_t (*close)(void *);
} *spi;
static const efi_guid spi_guid={0x4c7ffd28,0x6a06,0x4425,{0x9e,0xe2,0x67,0x6e,0xbc,0x08,0x96,0x83}};
struct pointer_state {uint64_t x,y,z;uint32_t buttons;};
struct pointer_mode {uint64_t minx,miny,minz,maxx,maxy,maxz;uint32_t attributes;};
struct pointer_protocol {
    efi_status (*reset)(void *,uint8_t);
    efi_status (*get_state)(void *,struct pointer_state *);
    void *wait;
    struct pointer_mode *mode;
};
static struct pointer_state pointer;
static struct pointer_mode pointer_mode={0,0,0,65535,65535,0,0};
static struct pointer_protocol absolute;
static uint64_t ticks(void) {uint64_t v;__asm__ volatile("mrs %0,cntvct_el0":"=r"(v));return v;}
static uint64_t hz(void) {uint64_t v;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(v));return v;}
static void publish(void) {clean((uintptr_t)&shared,(uintptr_t)&shared+64);}
static void delay(void *unused,unsigned us) {(void)unused;bs->stall(us);}
static int read_bus(void *unused,uint8_t *p,unsigned n) {
    uint8_t dummy[DADA_TCM_PAYLOAD+3];uint32_t cfg[13]={0};(void)unused;
    if(gpio_open)return dada_gpio_uefi_read(p,n);
    if (!spi_handle || !n || n>sizeof(dummy))return -1;
    memset(dummy,0xff,n);cfg[5]=1000000;cfg[7]=1;cfg[10]=1;cfg[11]=8;
    uint32_t s=spi->transfer(spi_handle,cfg,dummy,n,p,n);
    if (s)note("touch: SPI read status=%d bytes=%d\n",(uint64_t)s,(uint64_t)n);
    return s ? -1:0;
}
static int write_bus(void *unused,const uint8_t *p,unsigned n) {
    uint32_t cfg[13]={0};(void)unused;
    if(gpio_open)return dada_gpio_uefi_write(p,n);
    if (!spi_handle || !n || n>DADA_TCM_PAYLOAD+3)return -1;
    cfg[5]=1000000;cfg[7]=1;cfg[10]=1;cfg[11]=8;
    uint32_t s=spi->transfer(spi_handle,cfg,p,n,0,0);
    if (s)note("touch: SPI write status=%d bytes=%d\n",(uint64_t)s,(uint64_t)n);
    return s ? -1:0;
}
static const struct dada_tcm_bus bus={0,read_bus,write_bus,delay};
static void event(void *unused,unsigned type,unsigned id,unsigned x,unsigned y) {
    (void)unused;
    invalidate((uintptr_t)&shared.consumer,(uintptr_t)&shared.consumer+64);
    uint32_t at=shared.producer;
    if ((uint32_t)(at-shared.consumer)>=DADA_TOUCH_RING)shared.dropped++;
    else {
        struct dada_touch_event *e=&shared.events[at%DADA_TOUCH_RING];
        *e=(struct dada_touch_event){type,id,x,y};
        clean((uintptr_t)e,(uintptr_t)e+sizeof(*e));
        shared.producer=at+1;
    }
    publish();
}
static void pointer_update(void) {
    struct pointer_state next=pointer;next.buttons=0;
    for(unsigned i=0;i<DADA_TCM_CONTACTS;i++)if(tcm.contacts[i].down) {
        next.x=tcm.contacts[i].x;next.y=tcm.contacts[i].y;next.buttons=1;break;
    }
    if (next.x!=pointer.x || next.y!=pointer.y || next.buttons!=pointer.buttons) {
        pointer=next;pointer_changed=1;
        if(absolute.wait)bs->signal_event(absolute.wait);
    }
}
static efi_status pointer_reset(void *self,uint8_t extended) {
    (void)self;(void)extended;pointer_changed=0;return 0;
}
static efi_status pointer_get(void *self,struct pointer_state *out) {
    (void)self;if(!out)return EFI_ERROR_BIT|2;
    if(!pointer_changed)return EFI_ERROR_BIT|6;
    *out=pointer;pointer_changed=0;return 0;
}
static void pointer_wait(void *e,void *ctx) {
    (void)ctx;dada_uefi_touch_poll();if(pointer_changed)bs->signal_event(e);
}
/* Same GUID can describe different Qualcomm SPI ABIs: check the pinned dada thunks. */
static int verified_spi(void) {
    static const efi_guid loaded={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b}};
    efi_handle *handles=0;uint64_t count=0;int okay=0;
    if(!spi || spi->revision!=0x10000 || EFI_ERROR(bs->locate_handle_buffer(ByProtocol,&loaded,0,&count,&handles)))return 0;
    for(uint64_t i=0;i<count;i++) {
        uint8_t *li=0;
        if(EFI_ERROR(bs->handle_protocol(handles[i],&loaded,(void **)&li)) || !li)continue;
        uint64_t base=*(uint64_t *)(li+64),size=*(uint64_t *)(li+72);
        if(size>=0xc108 && (uintptr_t)spi==base+0xc0e8 &&
            (uintptr_t)spi->open==base+0x26d0 && (uintptr_t)spi->transfer==base+0x2784 &&
            (uintptr_t)spi->close==base+0x2714 && size>=0xd450) {
            okay=1;spi_image=(uint8_t *)(uintptr_t)base;
        }
    }
    bs->free_pool(handles);return okay;
}
static void spi_software_diagnostic(void) {
    if(!spi_image)return;
    /* Pinned-image data only, not peripheral registers or guessed pointers.
     * c150 is the DT API/config result; d2fc..d2ff are wrapper SE counts.
     * d300 is enum-initialized; d290 is the DT protocol pointer (printed only).
     */
    note("touch: SPI software config=%x enum=%d counts=%d,%d,%d,%d DTapi=%x\n",
        (uint64_t)*(volatile uint32_t *)(spi_image+0xc150),
        (uint64_t)*(volatile uint8_t *)(spi_image+0xd300),
        (uint64_t)*(volatile uint8_t *)(spi_image+0xd2fc),
        (uint64_t)*(volatile uint8_t *)(spi_image+0xd2fd),
        (uint64_t)*(volatile uint8_t *)(spi_image+0xd2fe),
        (uint64_t)*(volatile uint8_t *)(spi_image+0xd2ff),
        *(volatile uint64_t *)(spi_image+0xd290));
}
void dada_uefi_touch_setup(efi_boot_services *services,efi_handle parent,const void *driver,
    uint32_t bytes,dada_touch_log_fn log,dada_touch_cache_fn wb,dada_touch_cache_fn inv) {
    bs=services;note=log;clean=wb;invalidate=inv;
    memset(&shared,0,sizeof(shared));shared.magic=DADA_TOUCH_MAGIC;
    shared.version=DADA_TOUCH_VERSION;shared.size=sizeof(shared);
    efi_status s=bs->locate_protocol(&spi_guid,0,(void **)&spi);
    if(EFI_ERROR(s) && driver && bytes) {
        efi_handle image=0;s=bs->load_image(0,parent,0,(void *)driver,bytes,&image);
        if(!EFI_ERROR(s))s=bs->start_image(image,0,0);
        note("touch: load SPI driver status=%x\n",s);
        if(!EFI_ERROR(s))s=bs->locate_protocol(&spi_guid,0,(void **)&spi);
    }
    if(EFI_ERROR(s) || !verified_spi()) {
        spi=0;shared.state=DADA_TOUCH_FAILED;shared.error=1;
        note("touch: pinned dada SPI protocol unavailable status=%x\n",s);publish();return;
    }
    /* This is the reserved RAM XBL_DT region in dada's verified MemoryMapLib. */
    unsigned matches=0;
    for(unsigned off=0;off+40<=0x40000;off+=4) {
        uint8_t *p=(uint8_t *)(uintptr_t)(0x81a00000+off);
        if(p[0]!=0xd0 || p[1]!=0x0d || p[2]!=0xfe || p[3]!=0xed)continue;
        struct dada_touch_config found;
        if(!dada_touch_config_parse(p,0x40000-off,&found)) {config=found;matches++;}
    }
    note("touch: config matches=%d instance=%d enabled=%d pins=%d\n",
        (uint64_t)matches,(uint64_t)config.instance,(uint64_t)config.enabled,(uint64_t)config.pins_valid);
    note("touch: diagnostics r7; GPIO uses pinned firmware ID API; run touchprobe-gpio first\n");
    if(matches!=1) {shared.state=DADA_TOUCH_FAILED;shared.error=2;publish();return;}
    static const efi_guid abs_guid={0x8d59d32b,0xc655,0x4ae9,{0x9b,0x15,0xf2,0x59,0x04,0x99,0x2a,0x43}};
    absolute=(struct pointer_protocol){pointer_reset,pointer_get,0,&pointer_mode};
    s=bs->create_event(0x100,8,pointer_wait,0,&absolute.wait);
    if(!EFI_ERROR(s)) {
        efi_handle device=0;s=bs->install_protocol_interface(&device,&abs_guid,0,&absolute);
        if(EFI_ERROR(s)) {bs->close_event(absolute.wait);absolute.wait=0;}
    }
    note("touch: AbsolutePointer install=%x; SPI stays closed until requested\n",s);
    shared.state=DADA_TOUCH_DISCOVERED;publish();
}
void dada_uefi_touch_start(void) {
    if(active || gpio_open) {
        dada_uefi_touch_stop();
        if(gpio_open) {note("touch: restart refused; GPIO rollback incomplete\n");return;}
    }
    if((!spi && !gpio_preferred) || !config.dt || !config.pins_valid)return;
    memset(&shared.consumer,0,sizeof(shared)-64);
    shared.producer=shared.frames=shared.dropped=shared.error=0;
    memset(&tcm,0,sizeof(tcm));errors=0;
    uint32_t s=0;
    if(gpio_preferred) {
        /* Explicit selection survives a stop/restart when the same loader
         * subsequently launches XNU/iOS. No automatic takeover after SPI fails. */
        gpio_open=1;
        s=dada_gpio_uefi_start(bs,note) ? 3:0;
    } else {
    if(dada_touch_config_prepare(&config))return;
    clean((uintptr_t)config.dt,(uintptr_t)config.dt+config.bytes);
    static const efi_guid clock_guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
    struct dada_clock_protocol *clock=0;
    efi_status clock_status=bs->locate_protocol(&clock_guid,0,(void **)&clock);
    note("touch: clock protocol status=%x\n",clock_status);
    if(EFI_ERROR(clock_status) || dada_touch_clocks_start(&clocks,clock,note)) {
        shared.state=DADA_TOUCH_FAILED;shared.error=3;publish();return;
    }
    note("touch: SPI open instance=%d SE=0xa88000 pins=40,41,42,43\n",(uint64_t)config.instance);
    static const efi_guid dt_guid={0x0389b776,0x625f,0x11eb,{0x83,0xbe,0xc7,0x41,0xa9,0x13,0xde,0x34}};
    void *dt_api=0;int traced=0;
    efi_status dt_status=bs->locate_protocol(&dt_guid,0,&dt_api);
    if(!EFI_ERROR(dt_status) && spi_image)
        traced=!dada_dttrace_begin((uintptr_t *)(spi_image+0xd290),dt_api,note);
    note("touch: DT trace lookup=%x active=%d\n",dt_status,(uint64_t)traced);
    int fwtraced=spi_image && !dada_fwtrace_begin((uintptr_t *)(spi_image+0xd2f0),note);
    note("touch: SPI firmware trace active=%d\n",(uint64_t)fwtraced);
    s=spi->open(config.instance,&spi_handle);
    if(fwtraced)dada_fwtrace_end();
    if(traced)dada_dttrace_end();
    note("touch: SPI open result=%d handle=%x\n",(uint64_t)s,(uint64_t)(uintptr_t)spi_handle);
    spi_software_diagnostic();
    }
    int error=s || (!gpio_open && !spi_handle) ? DADA_TCM_IO:0;
    struct dada_tcm_message msg={0};
    if(!error) {
        error=dada_tcm_command(&bus,2,0,0,&msg);
        if(!error)error=dada_tcm_identify(&tcm,msg.data,msg.length);
        note("touch: IDENTIFY error_code=%x code=%x bytes=%d part=%s\n",(uint64_t)(uint32_t)error,
            (uint64_t)msg.code,(uint64_t)msg.length,tcm.part);
    } else note("touch: IDENTIFY skipped; SPI unavailable\n");
    if(!error)error=dada_tcm_command(&bus,0x20,0,0,&msg);
    if(!error)error=dada_tcm_application(&tcm,msg.data,msg.length);
    uint8_t report=DADA_TCM_TOUCH,report_config[128]={0};
    if(!error)error=dada_tcm_command(&bus,6,&report,1,&msg);
    memcpy(report_config,dada_tcm_report_config,sizeof(dada_tcm_report_config));
    if(!error)error=dada_tcm_command(&bus,0x26,report_config,tcm.max_config,&msg);
    if(!error)error=dada_tcm_command(&bus,0x25,0,0,&msg);
    if(!error) {
        if(msg.length!=tcm.max_config)error=DADA_TCM_PROTOCOL;
        else for(unsigned i=0;i<msg.length;i++)if(msg.data[i]!=report_config[i])error=DADA_TCM_PROTOCOL;
    }
    if(!error)error=dada_tcm_command(&bus,5,&report,1,&msg);
    if(error) {
        if(spi_handle)spi->close(spi_handle);spi_handle=0;
        if(gpio_open && !dada_gpio_uefi_stop())gpio_open=0;
        dada_touch_clocks_stop(&clocks,note);
        shared.state=DADA_TOUCH_FAILED;shared.error=(uint32_t)error;
        note("touch: inactive error_code=%x; no physical input claimed\n",(uint64_t)(uint32_t)error);
    } else {
        active=1;last_poll=0;shared.state=DADA_TOUCH_READY;
        note("touch: READY x=%d y=%d slots=%d\n",(uint64_t)tcm.max_x,(uint64_t)tcm.max_y,(uint64_t)tcm.objects);
    }
    clean((uintptr_t)&shared,(uintptr_t)&shared+sizeof(shared));
}
void dada_uefi_touch_start_gpio(void) {gpio_preferred=1;dada_uefi_touch_start();}
void dada_uefi_touch_probe_gpio(void) {
    if(active || gpio_open) {note("touch: GPIO preflight refused while touch is active\n");return;}
    int error=dada_gpio_uefi_probe(bs,note);
    note("touch: GPIO preflight result=%x\n",(uint64_t)(uint32_t)error);
}
void dada_uefi_touch_stop(void) {
    active=0;dada_tcm_release(&tcm,event,0);pointer_update();
    if(spi_handle) {uint32_t s=spi->close(spi_handle);note("touch: SPI close=%d\n",(uint64_t)s);spi_handle=0;}
    if(gpio_open && !dada_gpio_uefi_stop())gpio_open=0;
    dada_touch_clocks_stop(&clocks,note);
    shared.state=DADA_TOUCH_STOPPED;publish();
}
void dada_uefi_touch_poll(void) {
    if(!active || polling || ticks()-last_poll<hz()/125)return;
    last_poll=ticks();polling=1;
    invalidate((uintptr_t)&shared.consumer,(uintptr_t)&shared.consumer+64);
    if(shared.stop) {dada_uefi_touch_stop();polling=0;return;}
    struct dada_tcm_message msg;
    int error=dada_tcm_read_message(&bus,&msg);
    if(!error && msg.code==DADA_TCM_IDENTIFY)error=DADA_TCM_RESTARTED;
    if(!error && msg.code==DADA_TCM_TOUCH) {
        error=dada_tcm_frame(&tcm,msg.data,msg.length,event,0);
        if(!error) {shared.frames++;pointer_update();}
    }
    if(error) {
        dada_tcm_release(&tcm,event,0);pointer_update();
        if(++errors>=8 || error==DADA_TCM_RESTARTED) {
            dada_uefi_touch_stop();shared.state=DADA_TOUCH_FAILED;shared.error=(uint32_t)error;
            note("touch: report failure_code=%x\n",(uint64_t)(uint32_t)error);
        }
    } else errors=0;
    publish();polling=0;
}
uint32_t dada_uefi_touch_state(void) {return shared.state;}
uint64_t dada_uefi_touch_shared_pa(void) {return (uintptr_t)&shared;}
const void *dada_uefi_touch_dt(uint32_t *size) {
    if(config.dt) {*size=config.bytes;return config.dt;}
    for(unsigned off=0;off+40<=0x40000;off+=4) {
        const uint8_t *p=(const uint8_t *)(uintptr_t)(0x81a00000+off);
        if(p[0]!=0xd0 || p[1]!=0x0d || p[2]!=0xfe || p[3]!=0xed)continue;
        uint32_t n=(uint32_t)p[4]<<24|(uint32_t)p[5]<<16|(uint32_t)p[6]<<8|p[7];
        if(n>=40 && n<=0x40000-off) {*size=n;return p;}
    }
    *size=0;return 0;
}
