/*
 * BQ27Z561 standard-register read port from Apache Mynewt (Apache-2.0),
 * commit 5bd9f465e2e6172f345985460bb1a55f472677d0.
 * See third-party LICENSE/NOTICE and the pinned source manifest in this release.
 * New transport: dada's ABI-verified I2CDxe, QUP1/SE5, 7-bit address 0x55.
 * No reset, unseal, manufacturer commands, data-flash or charger-control API.
 */
#include "dada_battery_uefi.h"
#ifndef DADA_BQ_HOST_TEST
#include "dada_i2c_driver.h"
#endif
#define NOT_READY (EFI_ERROR_BIT | 6)
#define UNSUPPORTED (EFI_ERROR_BIT | 3)
#define INVALID_DATA (EFI_ERROR_BIT | 33)
#define DEVICE_ERROR (EFI_ERROR_BIT | 7)
static struct dada_i2c_protocol *i2c;
static void *handle;
static efi_handle parent;
static void (*trace)(const char *);
static const char *phase="idle";
static efi_status backend=NOT_READY;
static uint16_t raw[5]; /* SOC, voltage, signed current, temperature, flags */
static uint32_t diagnostic[DADA_BQ_DIAG_SIZE];
static int attempted;
static void set_phase(const char *s) { phase=s; if(trace)trace(s); }
void dada_battery_set_parent(efi_handle p) { parent=p; }
void dada_battery_set_trace(void (*f)(const char *)) { trace=f; }
const char *dada_battery_phase(void) { return phase; }
efi_status dada_battery_backend_status(void) { return backend; }
uint32_t dada_battery_raw(unsigned n) { return n<5 ? raw[n]:0xffffffffu; }
uint32_t dada_battery_diag(unsigned n) { return n<DADA_BQ_DIAG_SIZE ? diagnostic[n]:0xffffffffu; }
_Static_assert(offsetof(struct dada_i2c_protocol,read)==16,"Read ABI");
_Static_assert(offsetof(struct dada_i2c_protocol,close)==40,"Close ABI");
_Static_assert(sizeof(struct dada_i2c_config)==24,"Config ABI");

static void unknown(struct dada_battery_sample *s) {
    *s=(struct dada_battery_sample){0,-1,-1,0,-273,0xffffffffu,0xffffffffu};
}
/* Five mandatory words must form a physically credible, initialized sample.
 * SOC is range checked before narrowing; 0xffff and all-zero buffers fail. */
efi_status dada_battery_decode_bq(const uint16_t w[5],struct dada_battery_sample *s) {
    unknown(s);
    int current=(int16_t)w[2];
    if(w[0]>100 || w[1]<2500 || w[1]>5500 || current < -30000 || current>30000 ||
        w[3]<2331 || w[3]>3731 || w[4]==0xffff || (w[4]&0x80))return INVALID_DATA;
    /* TI SLUUBO7 section 8.6 / SLUUC54: INIT bit7 is 0=complete,
     * 1=active. A set INIT bit must not be treated as ready. */
    s->soc_percent=w[0];s->voltage_mv=w[1];s->current_ma=current;
    /* Standard unit is 0.1 K. Use exact 273.15 K, rounded to nearest C. */
    int centi=(int)w[3]*10-27315;
    s->temperature_c=(centi+(centi<0 ? -50:50))/100;
    s->flags=DADA_BATTERY_VALID|DADA_BATTERY_PRESENT|DADA_BATTERY_VOLTAGE_VALID;
    /* DSG proves not charging, but its absence proves neither charging nor
     * external power. Keep external-power state unknown in every gauge sample. */
    if(w[4]&0x40)s->flags|=DADA_BATTERY_CHARGING_KNOWN;
    return 0;
}

/* The same DT API/thunks are used by the already verified dada SPI driver.
 * Read live firmware configuration, never patch it or try alternate buses. */
static int board_instance(efi_boot_services *bs,uint32_t *instance) {
    static const efi_guid guid={0x0389b776,0x625f,0x11eb,{0x83,0xbe,0xc7,0x41,0xa9,0x13,0xde,0x34}};
    uintptr_t *dt=0;uint64_t node[4]={0};uint32_t base=0,offset=0,flags=0;
    uint8_t count=0,count1=0,index=0;char status[16]={0};
    typedef int32_t (*path_fn)(void *,const char *);
    typedef int32_t (*scalar_fn)(void *,const char *,void *);
    typedef int32_t (*string_fn)(void *,const char *,char *,uint32_t);
    backend=bs->locate_protocol(&guid,0,(void **)&dt);
    if(EFI_ERROR(backend) || !dt || !dt[6] || !dt[19] || !dt[20] || !dt[25])return 0;
    path_fn path=(path_fn)dt[6];scalar_fn u8=(scalar_fn)dt[25],u32=(scalar_fn)dt[20];
    string_fn str=(string_fn)dt[19];
    if(path(node,"/soc/TOP_QUP_0") || u8(node,"num_se",&count) || count!=10 ||
       path(node,"/soc/TOP_QUP_1") || u8(node,"num_se",&count1) || count1!=8 ||
       u32(node,"se_wrapper_base_addr",&base) || base!=0xa80000 ||
       path(node,"/soc/TOP_QUP_1/TOP_QUP_1_SE_5") ||
       u32(node,"core_offset",&offset) || offset!=0x14000 ||
       u8(node,"se_index",&index) || index!=5 ||
       u32(node,"se_flags",&flags) || flags!=0x10000b8 ||
       str(node,"status",status,sizeof(status)) ||
       status[0]!='o' || status[1]!='k' || status[2]!='a' || status[3]!='y' || status[4])return 0;
    *instance=count+index+1;return 1;
}
static int verified_i2c(efi_boot_services *bs) {
#ifdef DADA_BQ_HOST_TEST
    (void)bs;return i2c && i2c->revision==0x10000;
#else
    static const efi_guid loaded={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b}};
    efi_handle *handles=0;uint64_t count=0;int good=0;
    if(!i2c || i2c->revision!=0x10000 ||
        EFI_ERROR(bs->locate_handle_buffer(ByProtocol,&loaded,0,&count,&handles)))return 0;
    for(uint64_t n=0;n<count;n++) {
        uint8_t *li=0;
        if(EFI_ERROR(bs->handle_protocol(handles[n],&loaded,(void **)&li)) || !li)continue;
        uint64_t base=*(uint64_t *)(li+64),size=*(uint64_t *)(li+72);
        if(size>=0xe000 && (uintptr_t)i2c==base+0xc0e8 &&
            (uintptr_t)i2c->open==base+0x2708 && (uintptr_t)i2c->read==base+0x27c0 &&
            (uintptr_t)i2c->close==base+0x274c &&
            *(uint32_t *)(uintptr_t)(base+0x2834)==0x528000aa &&
            *(uint32_t *)(uintptr_t)(base+0x2844)==0x52800169) {
            /* Compare the complete Read/Transfer/FIFO and platform/core paths.
             * These ranges contain no PE relocations. Merely matching protocol
             * slot addresses is insufficient for reading private handle fields. */
            good=1;
            static const uint32_t ranges[][2]={{0x2708,0x3510},{0x5098,0x6f00}};
            for(unsigned r=0;r<2;r++)
                for(uint32_t p=ranges[r][0];p<ranges[r][1];p++)
                    if(*(const uint8_t *)(uintptr_t)(base+p)!=dada_i2c_driver[p])good=0;
        }
    }
    bs->free_pool(handles);return good;
#endif
}
efi_status dada_battery_bind(efi_boot_services *bs) {
    static const efi_guid guid={0xb27ae8b1,0x3e10,0x4d07,{0xab,0x5c,0xeb,0x9a,0x6d,0xc6,0xfa,0x8f}};
    uint32_t instance=0;
    if(attempted)return handle ? 0:NOT_READY;
    attempted=1;
    for(unsigned n=0;n<5;n++)raw[n]=0xffff;
    for(unsigned n=0;n<DADA_BQ_DIAG_SIZE;n++)diagnostic[n]=0xffffffffu;
    set_phase("validate-q1-se5-config");
    if(!board_instance(bs,&instance))return UNSUPPORTED;
    set_phase("locate-i2c-protocol");
    efi_status status=bs->locate_protocol(&guid,0,(void **)&i2c);
#ifndef DADA_BQ_HOST_TEST
    if(status==(EFI_ERROR_BIT|14) && parent) {
        efi_handle image=0;
        set_phase("load-pinned-i2c-dxe");
        status=bs->load_image(0,parent,0,(void *)dada_i2c_driver,sizeof(dada_i2c_driver),&image);
        if(EFI_ERROR(status))return status;
        set_phase("start-pinned-i2c-dxe");
        status=bs->start_image(image,0,0);
        if(EFI_ERROR(status))return status;
        status=bs->locate_protocol(&guid,0,(void **)&i2c);
    }
#endif
    if(EFI_ERROR(status))return status;
    set_phase("verify-i2c-abi");
    if(!verified_i2c(bs))return UNSUPPORTED;
    set_phase("open-q1-se5-instance16");
    backend=i2c->open(instance,&handle);
    if(backend || !handle) {handle=0;return DEVICE_ERROR;}
    set_phase("bound-bq27z561");return 0;
}

static void capture_engine(void) {
    /* Open allocates a 0x88-byte handle (RVA 0x6124). Offsets are confirmed in
     * the pinned code compared by verified_i2c(). Only allocated RAM is read;
     * no register accesses or modifications of vendor state are made. */
    const volatile uint8_t *h=handle;
    diagnostic[DADA_BQ_RESULT]=*(const volatile uint32_t *)(h+0x30);
    diagnostic[DADA_BQ_STATE]=*(const volatile uint32_t *)(h+0x48);
    diagnostic[DADA_BQ_TOTAL]=*(const volatile uint32_t *)(h+0x4c);
    diagnostic[DADA_BQ_INDEX]=*(const volatile uint32_t *)(h+0x50);
    const volatile uint8_t *context=*(const volatile uint8_t *const volatile *)h;
    if(context) {
        const volatile uint8_t *config=*(const volatile uint8_t *const volatile *)context;
        if(config)diagnostic[DADA_BQ_CONFIG_FLAGS]=*(const volatile uint32_t *)(config+0xdc);
    }
    const volatile uint32_t *fifo=*(const volatile uint32_t *const volatile *)(h+0x60);
    if(fifo) {
        /* FIFO state struct starts with uint16 TX count, uint16 RX count,
         * uint32 error, as used at 0x3334/0x335c. */
        diagnostic[DADA_BQ_TX_COUNT]=*(const volatile uint16_t *)fifo;
        diagnostic[DADA_BQ_RX_COUNT]=*((const volatile uint16_t *)fifo+1);
        diagnostic[DADA_BQ_HW_ERROR]=fifo[1];
    }
}

static int read_complete(uint32_t count) {
    /* In this pinned FIFO implementation 0x3290 accumulates TX(1)+RX(2),
     * then 0x31f8 -> 0x2d84 adds the completed first descriptor length (1)
     * again. Read subtracts only one pointer byte: it returns 3 instead of 2.
     * r16b hardware evidence: result=0, state=5, index=2, total=4,
     * TX=0/RX=2 for the last descriptor, raw count=3, no FIFO error.
     * Require ALL completion evidence for this exact 1+2 byte transaction.
     * Never accept a partial read merely because its status was success. */
    return count==3 && diagnostic[DADA_BQ_RESULT]==0 &&
        diagnostic[DADA_BQ_STATE]==5 && diagnostic[DADA_BQ_TOTAL]==4 &&
        diagnostic[DADA_BQ_INDEX]==2 && diagnostic[DADA_BQ_TX_COUNT]==0 &&
        diagnostic[DADA_BQ_RX_COUNT]==2 && diagnostic[DADA_BQ_HW_ERROR]==0;
}
efi_status dada_battery_read(struct dada_battery_sample *s) {
    static const uint16_t regs[]={0x2c,0x08,0x0c,0x06,0x0a};
    static const char *phases[]={"read-bq-soc","read-bq-voltage","read-bq-current","read-bq-temperature","read-bq-flags"};
    struct dada_i2c_config config={100,0x55,0,0,0,0};
    uint16_t words[5];unknown(s);
    if(!handle)return NOT_READY;
    for(unsigned n=0;n<5;n++)raw[n]=0xffff;
    for(unsigned n=0;n<5;n++) {
        uint8_t bytes[2]={0xff,0xff};uint32_t count=0;
        set_phase(phases[n]);
        /* Pinned Read creates START|WRITE (1 byte pointer), then
         * repeated START|READ|STOP (2 bytes). Firmware ignores timeout_ms;
         * FIFO implementation polls in 10 us steps, bounded ~409.6 ms. */
        backend=i2c->read(handle,&config,regs[n],1,bytes,2,&count,20);
        diagnostic[DADA_BQ_REG]=regs[n];
        diagnostic[DADA_BQ_COUNT]=count;
        diagnostic[DADA_BQ_BYTES]=(uint32_t)bytes[0]|(uint32_t)bytes[1]<<8;
        capture_engine();
        if(backend || !read_complete(count)) {
            /* Stop on the first transport error; no repeated blocking retry. */
            handle=0;return DEVICE_ERROR;
        }
        words[n]=(uint16_t)bytes[0]|(uint16_t)bytes[1]<<8;raw[n]=words[n];
    }
    set_phase("decode-bq27z561");
    efi_status status=dada_battery_decode_bq(words,s);
    if(!EFI_ERROR(status))set_phase("valid-bq27z561");
    return status;
}
