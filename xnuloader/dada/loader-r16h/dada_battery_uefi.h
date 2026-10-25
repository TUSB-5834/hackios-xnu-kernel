/* Read-only dada BQ27Z561 telemetry, adapted from Apache Mynewt. */
#ifndef DADA_BATTERY_UEFI_H
#define DADA_BATTERY_UEFI_H
#include "efi.h"
struct dada_battery_sample {
    uint32_t flags;
    int32_t soc_percent, voltage_mv, current_ma, temperature_c;
    uint32_t power_state, charging_source;
};
#define DADA_BATTERY_VALID (1u << 0)
#define DADA_BATTERY_PRESENT (1u << 1)
#define DADA_BATTERY_EXTERNAL (1u << 2)
#define DADA_BATTERY_VOLTAGE_VALID (1u << 3)
#define DADA_BATTERY_CHARGING (1u << 4)
/* Bit 5 belongs to the old voltage-estimation ABI; never set it. */
#define DADA_BATTERY_EXTERNAL_KNOWN (1u << 6)
#define DADA_BATTERY_CHARGING_KNOWN (1u << 7)

/* Pinned dada I2CDxe Read wrapper; its public header's Transfer descriptor
 * layout is incompatible, so no Transfer/Write method is exposed here. */
struct dada_i2c_config {
    uint32_t frequency_khz, address, mode, stretch_us, config1, config2;
};
struct dada_i2c_protocol {
    uint64_t revision;
    uint32_t (EFIAPI *open)(uint32_t instance, void **handle);
    uint32_t (EFIAPI *read)(void *, struct dada_i2c_config *, uint16_t offset,
        uint16_t offset_length, uint8_t *, uint16_t length, uint32_t *, uint32_t timeout_ms);
    void *unused_write, *unused_transfer;
    uint32_t (EFIAPI *close)(void *);
};
efi_status dada_battery_bind(efi_boot_services *);
void dada_battery_set_parent(efi_handle);
void dada_battery_set_trace(void (*)(const char *));
const char *dada_battery_phase(void);
efi_status dada_battery_backend_status(void);
efi_status dada_battery_read(struct dada_battery_sample *);
efi_status dada_battery_decode_bq(const uint16_t words[5], struct dada_battery_sample *);
uint32_t dada_battery_raw(unsigned index);
/* Cached diagnostics only; querying these never starts another bus transaction.
 * Values are UINT32_MAX until captured. Raw bytes are not validated telemetry. */
enum dada_battery_diag_field {
    DADA_BQ_REG, DADA_BQ_COUNT, DADA_BQ_BYTES, DADA_BQ_RESULT,
    DADA_BQ_STATE, DADA_BQ_TOTAL, DADA_BQ_INDEX, DADA_BQ_CONFIG_FLAGS,
    DADA_BQ_TX_COUNT, DADA_BQ_RX_COUNT, DADA_BQ_HW_ERROR, DADA_BQ_DIAG_SIZE
};
uint32_t dada_battery_diag(unsigned index);
#endif
