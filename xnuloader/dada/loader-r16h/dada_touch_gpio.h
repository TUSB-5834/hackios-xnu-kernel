/* Mode-0 software SPI, adapted from UniXNU-pong's bitbang transfer.
 * Board operations are supplied by the dada UEFI adapter, never by XNU.
 */
#ifndef DADA_TOUCH_GPIO_H
#define DADA_TOUCH_GPIO_H
#include <stdint.h>
#define DADA_GPIO_SPI_MAX 1027
struct dada_gpio_ops {
    void *context;
    /* Logical config: mux[3:0], direction[4], pull[6:5], drive[18:7]
     * in 0.01 mA, eGPIO[19]. Latch is 0/1; these are not raw registers.
     */
    int (*save)(void *,unsigned,uint32_t *,uint32_t *);
    int (*configure)(void *,unsigned,unsigned,unsigned,unsigned);
    int (*output)(void *,unsigned,unsigned);
    int (*input)(void *,unsigned,unsigned *);
    int (*restore)(void *,unsigned,uint32_t,uint32_t);
    void (*delay)(void *,unsigned);
    uint64_t (*time_us)(void *);
};
struct dada_gpio_spi {
    struct dada_gpio_ops ops;
    uint32_t config[6],latch[6];
    unsigned changed,ready;
};
int dada_gpio_spi_begin(struct dada_gpio_spi *,const struct dada_gpio_ops *);
int dada_gpio_spi_transfer(struct dada_gpio_spi *,const uint8_t *,uint8_t *,unsigned);
int dada_gpio_spi_end(struct dada_gpio_spi *);
#endif
