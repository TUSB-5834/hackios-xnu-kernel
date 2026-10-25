/* Bounded parser for dada's RAM XBL DT. No MMIO, allocation, or FDT resizing. */
#include "dada_touch_config.h"
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static int equal(const char *a, const char *b) {
    while (*a && *a==*b) {a++;b++;} return *a==*b;
}
static int zlen(const uint8_t *p, size_t max, size_t *n) {
    for (*n=0; *n<max; ++*n) if (!p[*n]) return 1; return 0;
}
/* Also accepts a phandle instead of a path, resolving it before a second walk. */
static uint8_t *property(void *data, size_t capacity, const char *want,
    const char *key, uint32_t *length, uint32_t handle, char resolved[256]) {
    uint8_t *dt=data; uint32_t total,so,ss,sto,sts;
    size_t at,end,pathlen=0,marks[32],depth=0; char path[256]={0};
    if (capacity<40 || be32(dt)!=0xd00dfeed) return 0;
    total=be32(dt+4);so=be32(dt+8);sto=be32(dt+12);sts=be32(dt+32);ss=be32(dt+36);
    if (total<40 || total>capacity || so<40 || sto<40 || so>total || sto>total ||
        ss>total-so || sts>total-sto || (so&3)) return 0;
    at=so;end=so+ss;
    while (at+4<=end) {
        uint32_t token=be32(dt+at); at+=4;
        if (token==1) {
            size_t n;
            if (depth==32 || !zlen(dt+at,end-at,&n) || n>127) return 0;
            marks[depth++]=pathlen;
            if (n) {
                if (pathlen+n+1>=sizeof(path)) return 0;
                path[pathlen++]='/';
                for (size_t i=0;i<n;i++)path[pathlen++]=(char)dt[at+i];
                path[pathlen]=0;
            }
            at=(at+n+1+3)&~(size_t)3;
        } else if (token==2) {
            if (!depth) return 0; pathlen=marks[--depth];path[pathlen]=0;
        } else if (token==3) {
            size_t n;uint32_t len,offset;
            if (!depth || at+8>end) return 0;
            len=be32(dt+at);offset=be32(dt+at+4);at+=8;
            if (len>end-at || offset>=sts || !zlen(dt+sto+offset,sts-offset,&n)) return 0;
            const char *name=(const char *)dt+sto+offset;
            if (handle && len==4 && (equal(name,"phandle") || equal(name,"linux,phandle")) && be32(dt+at)==handle) {
                for(size_t i=0;i<=pathlen;i++)resolved[i]=path[i]; return dt+at;
            }
            if (!handle && equal(path,want) && equal(name,key)) {*length=len;return dt+at;}
            at=(at+len+3)&~(size_t)3;
        } else if (token==9) return 0;
        else if (token!=4) return 0;
    }
    return 0;
}
static int cell(void *dt,size_t size,const char *path,const char *key,uint32_t *value) {
    uint32_t n=0;uint8_t *p=property(dt,size,path,key,&n,0,0);
    if (!p || n!=4) return 0; *value=be32(p);return 1;
}
static int engines(void *dt,size_t size,const char *path,uint32_t *value) {
    uint32_t n=0;uint8_t *p=property(dt,size,path,"num_se",&n,0,0);
    if(!p || (n!=1 && n!=4))return 0;*value=n==1 ? p[0]:be32(p);return 1;
}
static uint8_t *pins(void *dt,size_t size,const char *se,const char *name,uint32_t flags) {
    uint32_t h,n=0;char path[256];
    if (!cell(dt,size,se,name,&h) || !property(dt,size,0,0,&n,h,path))return 0;
    uint8_t *p=property(dt,size,path,"config",&n,0,0);
    if (!p || n!=32)return 0;
    uint32_t first=be32(p);
    if (first!=30 && first!=40)return 0;
    for (unsigned i=0;i<4;i++)if(be32(p+8*i)!=first+i || be32(p+8*i+4)!=flags)return 0;
    return p;
}
int dada_touch_config_parse(void *dt,size_t size,struct dada_touch_config *c) {
    static const char se[]="/soc/TOP_QUP_1/TOP_QUP_1_SE_2";
    uint32_t count,base,offset,n=0,count1;
    *c=(struct dada_touch_config){0};
    if (!engines(dt,size,"/soc/TOP_QUP_0",&count) || count==0 || count>10 ||
        !engines(dt,size,"/soc/TOP_QUP_1",&count1) || count1<3 || count1>10 ||
        !cell(dt,size,"/soc/TOP_QUP_1","se_wrapper_base_addr",&base) || base!=0xa80000 ||
        !cell(dt,size,se,"core_offset",&offset) || offset!=0x8000) return -1;
    uint8_t *status=property(dt,size,se,"status",&n,0,0);
    if (!status || n<5 || n>9 || status[n-1])return -2;
    if (!equal((char *)status,"okay") && !equal((char *)status,"disabled"))return -2;
    c->dt=dt;c->bytes=be32((uint8_t *)dt+4);c->instance=count+3;c->status=status;
    c->status_bytes=n;
    c->enabled=equal((char *)status,"okay");
    c->active=pins(dt,size,se,"pinctrl-2",0x2580011);
    c->sleep=pins(dt,size,se,"pinctrl-3",0xc80021);
    c->pins_valid=c->active!=0 && c->sleep!=0;
    return 0;
}
int dada_touch_config_prepare(struct dada_touch_config *c) {
    if (!c->dt || !c->pins_valid || !c->status || c->status_bytes<5 || c->status_bytes>9)return -1;
    /* Fixed-size RAM edits of the already validated SPI active/sleep groups. */
    for(unsigned i=0;i<4;i++) {c->active[8*i+3]=(uint8_t)(40+i);c->sleep[8*i+3]=(uint8_t)(40+i);}
    /* Preserve property length but clear the entire old "disabled" string.
     * Leaving "okay\0led\0" is not a valid single-string DT property.
     */
    for(unsigned i=0;i<c->status_bytes;i++)c->status[i]=0;
    c->status[0]='o';c->status[1]='k';c->status[2]='a';c->status[3]='y';
    c->enabled=1;return 0;
}
