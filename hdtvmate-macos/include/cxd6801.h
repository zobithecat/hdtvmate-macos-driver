#ifndef CXD6801_H
#define CXD6801_H

#include "hdtvmate.h"
#include "it9300.h"

/*
 * Sony CXD6801 ATSC 3.0 Demodulator Driver
 *
 * The CXD6801 is Sony's integrated ATSC 3.0 demodulator/tuner IC.
 * It communicates via I2C through the IT9300 USB bridge.
 *
 * Features:
 * - ATSC 3.0 (A/321, A/322) demodulation
 * - ATSC 1.0 (A/53) demodulation
 * - J.83B (QAM) demodulation
 * - Integrated ASCOT3 tuner front-end
 * - ALP (ATSC Link-layer Protocol) output
 * - Channel bonding support
 * - PLP (Physical Layer Pipe) management
 *
 * Register access pattern (from CXD2880 Linux driver):
 * - Bank select: write to reg 0x00 to select register bank
 * - Then read/write registers within that bank
 * - I2C address typically 0x6C (demod) and 0x60 (tuner)
 */

/* CXD6801 I2C addresses (8-bit format, as used by Endeavour/IT9300)
 * Confirmed by I2C bus scan: Bus=3, Addr=0xC8 responds!
 * Valid addresses from sony_cxd6801_demod_Create: 0xC8, 0xCA, 0xD8, 0xDA
 */
#define CXD6801_I2C_ADDR_WRITE  0xC8   /* Demod WRITE address - CONFIRMED */
#define CXD6801_I2C_ADDR_READ   0xDC   /* Demod READ address - CONFIRMED */
#define CXD6801_I2C_ADDR_DEMOD  0xC8   /* Alias for write addr */
#define CXD6801_I2C_ADDR_TUNER  0xC2   /* ASCOT3 tuner */
#define CXD6801_I2C_BUS         0x03   /* I2C bus number - CONFIRMED */

/* Chip IDs */
#define CXD6801_CHIP_ID         0x6801
#define CXD6802_CHIP_ID         0x6802

/* Demodulator state */
typedef enum {
    CXD6801_STATE_UNKNOWN = 0,
    CXD6801_STATE_SLEEP,
    CXD6801_STATE_ACTIVE_ATSC3,
    CXD6801_STATE_ACTIVE_ATSC1,
    CXD6801_STATE_ACTIVE_J83B,
    CXD6801_STATE_SHUTDOWN,
} cxd6801_state_t;

/* ATSC 3.0 bandwidth */
typedef enum {
    CXD6801_BW_6MHZ = 6,
    CXD6801_BW_7MHZ = 7,
    CXD6801_BW_8MHZ = 8,
} cxd6801_bandwidth_t;

/* PLP configuration */
typedef struct {
    uint8_t  num_plps;
    uint8_t  plp_ids[64];
    bool     plp_valid[64];
} cxd6801_plp_config_t;

/* ATSC 3.0 L1 Basic info */
typedef struct {
    uint8_t  version;
    uint8_t  mimo;
    uint8_t  lls_flag;
    uint8_t  time_info;
    uint8_t  papr_reduction;
    uint8_t  frame_length_mode;
    uint16_t frame_length;
    uint8_t  num_subframes;
    uint8_t  num_plps;
    uint8_t  fft_size;
    uint8_t  guard_interval;
} cxd6801_l1_basic_t;

/* ATSC 3.0 Bootstrap info */
typedef struct {
    uint8_t  version;
    uint8_t  ea_wake_up;
    uint8_t  min_time_to_next;
    uint8_t  system_bandwidth;
    uint8_t  bsr_coefficient;
} cxd6801_bootstrap_t;

/* I2C communication layer (through IT9300) */
typedef struct {
    it9300_device_t *bridge;
    uint8_t          chip_idx;     /* RX port index (0-3) */
    uint8_t          i2c_addr;     /* I2C slave address */
    uint8_t          i2c_bus;
} cxd6801_i2c_t;

/* CXD6801 device context */
typedef struct {
    cxd6801_i2c_t    i2c_demod;
    cxd6801_i2c_t    i2c_tuner;
    it9300_device_t  *bridge;
    cxd6801_state_t  state;
    uint16_t         chip_id;
    uint32_t         frequency_khz;
    cxd6801_bandwidth_t bandwidth;
    cxd6801_plp_config_t plp_config;
    bool             initialized;
    bool             tuner_initialized;
} cxd6801_device_t;

/* --- I2C over IT9300 --- */
hdtvmate_error_t cxd6801_i2c_init(cxd6801_i2c_t *i2c, it9300_device_t *bridge,
                                   uint8_t chip_idx, uint8_t i2c_addr, uint8_t i2c_bus);
hdtvmate_error_t cxd6801_i2c_read(cxd6801_i2c_t *i2c, uint8_t bank,
                                   uint8_t reg, uint8_t *data, uint8_t len);
hdtvmate_error_t cxd6801_i2c_write(cxd6801_i2c_t *i2c, uint8_t bank,
                                    uint8_t reg, const uint8_t *data, uint8_t len);
hdtvmate_error_t cxd6801_i2c_write_one(cxd6801_i2c_t *i2c, uint8_t bank,
                                        uint8_t reg, uint8_t value);
hdtvmate_error_t cxd6801_i2c_set_bits(cxd6801_i2c_t *i2c, uint8_t bank,
                                       uint8_t reg, uint8_t mask, uint8_t value);

/* --- Demodulator core --- */
hdtvmate_error_t cxd6801_create(cxd6801_device_t *dev, it9300_device_t *bridge,
                                 uint8_t chip_idx);
hdtvmate_error_t cxd6801_initialize(cxd6801_device_t *dev);
hdtvmate_error_t cxd6801_read_chip_id(cxd6801_device_t *dev);
hdtvmate_error_t cxd6801_sleep(cxd6801_device_t *dev);
hdtvmate_error_t cxd6801_shutdown(cxd6801_device_t *dev);
hdtvmate_error_t cxd6801_soft_reset(cxd6801_device_t *dev);

/* --- ASCOT3 Tuner --- */
hdtvmate_error_t cxd6801_tuner_init(cxd6801_device_t *dev);
hdtvmate_error_t cxd6801_tuner_tune(cxd6801_device_t *dev, uint32_t frequency_khz,
                                     cxd6801_bandwidth_t bw);
hdtvmate_error_t cxd6801_tuner_sleep(cxd6801_device_t *dev);

/* --- ATSC 3.0 operations --- */
hdtvmate_error_t cxd6801_atsc3_tune(cxd6801_device_t *dev, uint32_t frequency_khz,
                                     cxd6801_bandwidth_t bw);
hdtvmate_error_t cxd6801_atsc3_tune_end(cxd6801_device_t *dev);
hdtvmate_error_t cxd6801_atsc3_check_demod_lock(cxd6801_device_t *dev, bool *locked);
hdtvmate_error_t cxd6801_atsc3_check_alp_lock(cxd6801_device_t *dev, bool *locked);
hdtvmate_error_t cxd6801_atsc3_wait_lock(cxd6801_device_t *dev, uint32_t timeout_ms);
hdtvmate_error_t cxd6801_atsc3_set_plp_config(cxd6801_device_t *dev,
                                                const uint8_t *plp_ids, uint8_t count);
hdtvmate_error_t cxd6801_atsc3_sleep(cxd6801_device_t *dev);

/* --- ATSC 1.0 operations --- */
hdtvmate_error_t cxd6801_atsc1_tune(cxd6801_device_t *dev, uint32_t frequency_khz);
hdtvmate_error_t cxd6801_atsc1_check_lock(cxd6801_device_t *dev, bool *locked);
hdtvmate_error_t cxd6801_atsc1_wait_lock(cxd6801_device_t *dev, uint32_t timeout_ms);

/* --- Signal monitoring --- */
hdtvmate_error_t cxd6801_monitor_sync_stat(cxd6801_device_t *dev, uint8_t *sync_stat);
hdtvmate_error_t cxd6801_monitor_snr(cxd6801_device_t *dev, int32_t *snr_x100);
hdtvmate_error_t cxd6801_monitor_avg_snr(cxd6801_device_t *dev, int32_t *snr_x100);
hdtvmate_error_t cxd6801_monitor_pre_ldpc_ber(cxd6801_device_t *dev,
                                               uint32_t *num, uint32_t *den);
hdtvmate_error_t cxd6801_monitor_pre_bch_ber(cxd6801_device_t *dev,
                                              uint32_t *num, uint32_t *den);
hdtvmate_error_t cxd6801_monitor_carrier_offset(cxd6801_device_t *dev, int32_t *offset_hz);
hdtvmate_error_t cxd6801_monitor_l1_basic(cxd6801_device_t *dev, cxd6801_l1_basic_t *l1);
hdtvmate_error_t cxd6801_monitor_bootstrap(cxd6801_device_t *dev, cxd6801_bootstrap_t *bs);
hdtvmate_error_t cxd6801_monitor_plp_list(cxd6801_device_t *dev,
                                           uint8_t *plp_ids, uint8_t *count);
hdtvmate_error_t cxd6801_monitor_rf_level(cxd6801_device_t *dev, int32_t *rf_dbm);
hdtvmate_error_t cxd6801_monitor_ifagc(cxd6801_device_t *dev, uint32_t *ifagc);

/* --- High-level integration --- */
hdtvmate_error_t cxd6801_acquire_channel(cxd6801_device_t *dev,
                                          uint32_t frequency_khz,
                                          broadcast_standard_t std);
hdtvmate_error_t cxd6801_acquire_channel_plp(cxd6801_device_t *dev,
                                              uint32_t frequency_khz,
                                              const uint8_t *plp_ids, uint8_t plp_count);
hdtvmate_error_t cxd6801_get_stats(cxd6801_device_t *dev, signal_stats_t *stats);

/* Cleanup */
void cxd6801_deinit(cxd6801_device_t *dev);

#endif /* CXD6801_H */
