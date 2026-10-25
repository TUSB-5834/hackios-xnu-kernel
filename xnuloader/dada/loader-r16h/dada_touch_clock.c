/* Qualcomm clock preparation informed by Project-SunUEFI PianoTouchProbe.c.
 * Independently adapted for dada; preserve clocks already enabled on entry.
 * See uefi/dada/touch/UPSTREAM.md. No MMIO or SCM operations are performed.
 */
#include "dada_touch_clock.h"
static const char *const names[5]={
    "gcc_qupv3_wrap1_core_clk","gcc_qupv3_wrap1_core_2x_clk",
    "gcc_qupv3_wrap_1_m_ahb_clk","gcc_qupv3_wrap_1_s_ahb_clk",
    "gcc_qupv3_wrap1_s2_clk"
};
void dada_touch_clocks_stop(struct dada_touch_clocks *c,dada_clock_log_fn log) {
    if(!c->protocol)return;
    for(unsigned i=5;i--;)if(c->held&(1u<<i)) {
        efi_status s=c->protocol->disable(c->protocol,c->ids[i]);
        log("touch: clock release %s status=%x\n",names[i],s);
        /* Keep ownership on failure; do not issue another enable in that case. */
        if(!EFI_ERROR(s))c->held&=~(1u<<i);
    }
    if(!c->held)c->protocol=0;
}
int dada_touch_clocks_start(struct dada_touch_clocks *c,struct dada_clock_protocol *p,dada_clock_log_fn log) {
    if(c->held || c->protocol || !p || (p->version>>16)!=1 ||
        !p->get_id || !p->enable || !p->disable || !p->is_enabled)return -1;
    c->protocol=p;
    for(unsigned i=0;i<5;i++) {
        uint8_t enabled=0;
        efi_status s=p->get_id(p,names[i],&c->ids[i]);
        if(!EFI_ERROR(s))s=p->is_enabled(p,c->ids[i],&enabled);
        log("touch: clock query %s status=%x enabled=%d\n",names[i],s,(uint64_t)enabled);
        if(EFI_ERROR(s))goto failed;
        if(enabled)continue;
        s=p->enable(p,c->ids[i]);
        log("touch: clock enable %s status=%x\n",names[i],s);
        if(EFI_ERROR(s))goto failed;
        c->held|=1u<<i;
    }
    return 0;
failed:
    dada_touch_clocks_stop(c,log);return -1;
}
