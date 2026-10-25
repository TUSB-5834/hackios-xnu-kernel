/*
 * Software SPI edge sequence adapted from UniXNU-pong, commit
 * b4b6d6c71acab00a57292cc34e9b271773d7d7f8, pong-touch-console.patch.
 * That file credits HackiOS redwood_goodix.c (APSL 2.0); see UPSTREAM.md.
 * No Goodix registers, Pong addresses, power settings or pin swapping.
 */
#include "dada_touch_gpio.h"
/* Confirmed by dada-stock.dts mi_ts_spi_* and syna_tcm_* nodes. */
static const unsigned pins[6]={40,41,42,43,161,162};
enum { MISO,MOSI,CLK,CS,RESET,IRQ };
static int output(struct dada_gpio_spi *s,unsigned i,unsigned value) {
    return s->ops.output(s->ops.context,pins[i],value);
}
static int configure(struct dada_gpio_spi *s,unsigned i,unsigned dir,unsigned drive) {
    /* Mark before changing anything: a failing provider may have partly written. */
    s->changed|=1u<<i;
    return s->ops.configure(s->ops.context,pins[i],dir,0,drive);
}
static int driven(struct dada_gpio_spi *s,unsigned i,unsigned value) {
    s->changed|=1u<<i;
    /* Load the output latch before enabling output, avoiding a CS/reset glitch. */
    if(output(s,i,value))return -1;
    return configure(s,i,1,2); /* EFI drive code 2 = 6mA. */
}
int dada_gpio_spi_end(struct dada_gpio_spi *s) {
    if(!s || !s->changed)return 0;
    s->ready=0;
    /* Do not clock another bit while deselecting the controller. */
    if(s->changed&(1u<<CS))output(s,CS,1);
    if(s->changed&(1u<<CLK))output(s,CLK,0);
    for(unsigned i=6;i--;)if(s->changed&(1u<<i)) {
        if(!s->ops.restore(s->ops.context,pins[i],s->config[i],s->latch[i]))
            s->changed&=~(1u<<i);
    }
    return s->changed ? -1:0;
}
int dada_gpio_spi_begin(struct dada_gpio_spi *s,const struct dada_gpio_ops *ops) {
    if(!s || !ops || s->changed || s->ready || !ops->save || !ops->configure ||
        !ops->output || !ops->input || !ops->restore || !ops->delay || !ops->time_us)return -1;
    s->ops=*ops;
    /* Obtain every rollback value before the first write. Refuse eGPIO ownership
     * or an unrelated alternate function instead of taking an unknown pin. */
    for(unsigned i=0;i<6;i++) {
        if(ops->save(ops->context,pins[i],&s->config[i],&s->latch[i]))return -1;
        unsigned function=s->config[i]&15;
        if((s->config[i]&(1u<<19)) || function>(i<4 ? 1u:0u) || s->latch[i]>1)return -1;
    }
    if(driven(s,CS,1) || driven(s,CLK,0) || driven(s,MOSI,0) ||
        configure(s,MISO,0,0) || configure(s,IRQ,0,0) || driven(s,RESET,1))goto failed;
    /* Deassert dada's active-low reset; no reset pulse and no PMIC writes.
     * Power must already be supplied by firmware. */
    ops->delay(ops->context,200000);
    unsigned reset=0;
    if(ops->input(ops->context,pins[RESET],&reset) || reset!=1)goto failed;
    s->ready=1;return 0;
failed:
    dada_gpio_spi_end(s);return -1;
}
int dada_gpio_spi_transfer(struct dada_gpio_spi *s,const uint8_t *tx,uint8_t *rx,unsigned length) {
    if(!s || !s->ready || !length || length>DADA_GPIO_SPI_MAX || (!tx && !rx))return -1;
    uint64_t start=s->ops.time_us(s->ops.context);
    if(output(s,CLK,0) || output(s,CS,0))goto failed;
    s->ops.delay(s->ops.context,2);
    for(unsigned i=0;i<length;i++) {
        uint8_t out=tx ? tx[i]:0xff,in=0;
        for(int bit=7;bit>=0;bit--) {
            if(s->ops.time_us(s->ops.context)-start>500000)goto failed;
            if(output(s,MOSI,(out>>bit)&1))goto failed;
            s->ops.delay(s->ops.context,2);
            if(output(s,CLK,1))goto failed;
            s->ops.delay(s->ops.context,2);
            unsigned value=0;
            if(s->ops.input(s->ops.context,pins[MISO],&value) || value>1)goto failed;
            in=(uint8_t)((in<<1)|value);
            if(output(s,CLK,0))goto failed;
        }
        if(rx)rx[i]=in;
    }
    s->ops.delay(s->ops.context,2);
    if(output(s,CS,1))goto failed;
    return 0;
failed:
    /* A partial packet cannot be reused. Invalidate this session until stop. */
    s->ready=0;output(s,CS,1);output(s,CLK,0);return -1;
}
