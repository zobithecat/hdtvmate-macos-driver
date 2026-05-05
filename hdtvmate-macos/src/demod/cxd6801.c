#include "cxd6801.h"
#include <stdio.h>
#include <string.h>

/*
 * cxd6801.c - Sony CXD6801 demodulator core driver
 *
 * This implements the demodulator initialization and control.
 * Register sequences are derived from:
 * 1. Linux kernel CXD2880 driver (similar Sony register programming pattern)
 * 2. Symbol analysis of liba3_phy_sony.so (unstripped binary)
 * 3. Ghidra decompilation of key initialization functions
 *
 * TODO: The exact register initialization sequences need to be extracted
 * from the binary. The current code provides the framework with placeholder
 * register values that must be filled in from Ghidra analysis.
 *
 * Key register banks (from CXD2880 reference):
 * Bank 0x00: Common/control registers
 * Bank 0x10: ATSC 3.0 demod registers
 * Bank 0x20: ATSC 1.0 demod registers
 * Bank 0x60: Tuner (ASCOT3) registers
 */

/* External functions */
extern void br_user_delay(uint32_t ms);
extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);
extern uint64_t br_user_time_ms(void);

hdtvmate_error_t cxd6801_create(cxd6801_device_t *dev, it9300_device_t *bridge,
                                 uint8_t chip_idx)
{
    memset(dev, 0, sizeof(*dev));
    dev->bridge = bridge;
    dev->state = CXD6801_STATE_UNKNOWN;

    /* I2C addresses confirmed by bus scan:
     * Bus=3, Demod=0xC8
     * ASCOT3 tuner is INTEGRATED in CXD6801 — accessed via same demod I2C
     * (not a separate I2C slave). Tuner registers are in a separate bank
     * accessed through demod's register space. */
    uint8_t demod_addr = CXD6801_I2C_ADDR_DEMOD;  /* 0xC8 */
    uint8_t tuner_addr = CXD6801_I2C_ADDR_DEMOD;  /* Same as demod! Integrated tuner */

    cxd6801_i2c_init(&dev->i2c_demod, bridge, chip_idx, demod_addr, chip_idx);
    cxd6801_i2c_init(&dev->i2c_tuner, bridge, chip_idx, tuner_addr, chip_idx);

    LOG_INFO("CXD6801 created: demod_addr=0x%02x, tuner_addr=0x%02x, chip_idx=%d",
             demod_addr, tuner_addr, chip_idx);
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_read_chip_id(cxd6801_device_t *dev)
{
    uint8_t data[2] = {0};
    hdtvmate_error_t ret;

    /*
     * Read chip ID from demodulator.
     * From CXD2880 reference: Bank 0x00, reg 0xFD-0xFE contains chip ID
     * CXD6801 likely uses similar register layout.
     */
    /*
     * From sony_cxd6801_demod_ChipID disassembly:
     * chip_id = (reg[0xFB] & 0x03) << 8 | reg[0xFD]
     * Both from bank 0x00.
     */
    uint8_t reg_fb = 0, reg_fd = 0;

    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x00, 0xFB, &reg_fb, 1);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Failed to read reg 0xFB");
        return ret;
    }

    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x00, 0xFD, &reg_fd, 1);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Failed to read reg 0xFD");
        return ret;
    }

    LOG_INFO("Chip ID regs (addr=0x%02x): 0xFB=0x%02x, 0xFD=0x%02x => 0x%04x",
             dev->i2c_demod.i2c_addr, reg_fb, reg_fd,
             ((uint16_t)(reg_fb & 0x03) << 8) | reg_fd);
    dev->chip_id = ((uint16_t)(reg_fb & 0x03) << 8) | reg_fd;

    /* Chip ID 0x0396 is the actual CXD6801 silicon revision.
     * The CXD6801_CHIP_ID (0x6801) was a guess from CXD2880 patterns.
     * Accept 0x0396 as valid. */
    if (dev->chip_id == 0x0396) {
        LOG_INFO("Chip ID 0x0396 accepted (CXD6801 confirmed via bus scan)");
    } else if (dev->chip_id != CXD6801_CHIP_ID && dev->chip_id != CXD6802_CHIP_ID) {
        LOG_WARN("Unexpected chip ID: 0x%04x", dev->chip_id);
    }
    LOG_INFO("Chip ID: 0x%04x (%s)", dev->chip_id,
             dev->chip_id == CXD6801_CHIP_ID ? "CXD6801" :
             dev->chip_id == CXD6802_CHIP_ID ? "CXD6802" : "Unknown");

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_initialize(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;

    LOG_INFO("Initializing CXD6801 demodulator...");

    /* Read and verify chip ID */
    ret = cxd6801_read_chip_id(dev);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Chip ID read failed - check I2C connection");
        return ret;
    }

    if (dev->chip_id != CXD6801_CHIP_ID && dev->chip_id != CXD6802_CHIP_ID) {
        LOG_WARN("Unexpected chip ID: 0x%04x (expected 0x6801 or 0x6802)", dev->chip_id);
        /* Continue anyway - the register map might still be compatible */
    }

    /*
     * XtoSL - Unknown to Sleep state transition
     * From Ghidra decompile of XtoSL() at 0xe2a34 (chipId=0x396 branch).
     *
     * This sequence initializes the demod from power-on state to SLEEP.
     * CRITICAL: SLVX reg 0x02 = 0x00 may be the "write enable" command
     * that unlocks SLV-T register writes!
     *
     * All writes use SLVX (0xDC) address directly (flat, no bank).
     * SLVT (0xC8) writes happen after SLVX init (bank-based).
     */
    LOG_INFO("XtoSL: initializing demod (chipId=0x%04x)...", dev->chip_id);

    /* SLVX writes — direct register access via 0xDC, no bank select needed */
    /* These go directly to 0xDC as flat registers */
    {
        uint8_t tx[8];

        /* SLVX reg 0x02 = 0x00 — possibly "write enable" / power control */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_READ; tx[3]=0x02; tx[4]=0x00;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
        br_user_delay(4);

        /* SLVX reg 0x00 = 0x00 */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_READ; tx[3]=0x00; tx[4]=0x00;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);

        /* SLVX reg 0x1D = 0x00 */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_READ; tx[3]=0x1D; tx[4]=0x00;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);

        /* SLVX reg 0x14 = xtalFreq (0=20.5MHz, 1=24MHz, 2=41MHz) */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_READ; tx[3]=0x14; tx[4]=0x00;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
        br_user_delay(2);

        /* SLVX reg 0x50 = 0x00 */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_READ; tx[3]=0x50; tx[4]=0x00;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);

        /* SLVX reg 0x90 = 0x00 (conditional in Sony code, include it) */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_READ; tx[3]=0x90; tx[4]=0x00;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
        br_user_delay(2);

        /* SLVX reg 0x10 = 0x00 */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_READ; tx[3]=0x10; tx[4]=0x00;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
        br_user_delay(4);

        /* SLVT bank 0x95, reg 0x23 = 0x03 (for chipId 0x396) */
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_SLVT; tx[3]=0x00; tx[4]=0x95;
        br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
        tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_SLVT; tx[3]=0x23; tx[4]=0x03;
        ret = br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
    }

    if (ret != HDTVMATE_OK) {
        LOG_ERR("XtoSL failed");
        return ret;
    }
    LOG_INFO("XtoSL: done");

    /*
     * Step 3: Configure ALP/TS clock mode
     * From binary: sony_cxd6801_demod_SetALPClockModeAndFreq()
     * and sony_cxd6801_demod_SetTSClockModeAndFreq()
     *
     * TODO: These exact values need extraction from binary.
     * Placeholder values based on CXD2880 reference.
     */

    /* Step 4: Stream output configuration */
    /* sony_cxd6801_demod_SetStreamOutput() */

    dev->state = CXD6801_STATE_SLEEP;
    dev->initialized = true;

    /* Initialize tuner (ASCOT3) */
    ret = cxd6801_tuner_init(dev);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Tuner initialization failed");
        return ret;
    }

    LOG_INFO("CXD6801 initialized successfully (state: SLEEP)");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_sleep(cxd6801_device_t *dev)
{
    /* Transition from active to sleep */
    hdtvmate_error_t ret;

    if (dev->state == CXD6801_STATE_SLEEP) return HDTVMATE_OK;

    LOG_DBG("CXD6801 -> SLEEP");

    /* Call appropriate sleep function based on current state */
    if (dev->state == CXD6801_STATE_ACTIVE_ATSC3) {
        ret = cxd6801_atsc3_sleep(dev);
    } else {
        /* Generic sleep via register write */
        ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x10, 0x00);
    }

    if (ret == HDTVMATE_OK) {
        dev->state = CXD6801_STATE_SLEEP;
    }
    return ret;
}

hdtvmate_error_t cxd6801_shutdown(cxd6801_device_t *dev)
{
    LOG_INFO("CXD6801 shutdown");
    cxd6801_sleep(dev);
    dev->state = CXD6801_STATE_SHUTDOWN;
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_soft_reset(cxd6801_device_t *dev)
{
    /* sony_cxd6801_demod_SoftReset (confirmed from Ghidra):
     * Bank 0x00, reg 0xFE = 0x01 */
    hdtvmate_error_t ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xFE, 0x01);
    return ret;
}

/* High-level channel acquisition */
hdtvmate_error_t cxd6801_acquire_channel(cxd6801_device_t *dev,
                                          uint32_t frequency_khz,
                                          broadcast_standard_t std)
{
    hdtvmate_error_t ret;

    LOG_INFO("Acquiring channel: %u kHz, standard=%d", frequency_khz, std);

    /* Ensure we're in sleep state first */
    if (dev->state != CXD6801_STATE_SLEEP) {
        ret = cxd6801_sleep(dev);
        if (ret != HDTVMATE_OK) return ret;
    }

    switch (std) {
    case BROADCAST_ATSC3:
        ret = cxd6801_atsc3_tune(dev, frequency_khz, CXD6801_BW_6MHZ);
        if (ret != HDTVMATE_OK) return ret;
        ret = cxd6801_atsc3_wait_lock(dev, 3000);
        break;
    case BROADCAST_ATSC1:
        ret = cxd6801_atsc1_tune(dev, frequency_khz);
        if (ret != HDTVMATE_OK) return ret;
        ret = cxd6801_atsc1_wait_lock(dev, 3000);
        break;
    default:
        return HDTVMATE_ERR_INVALID_PARAM;
    }

    return ret;
}

hdtvmate_error_t cxd6801_acquire_channel_plp(cxd6801_device_t *dev,
                                              uint32_t frequency_khz,
                                              const uint8_t *plp_ids, uint8_t plp_count)
{
    hdtvmate_error_t ret;

    ret = cxd6801_acquire_channel(dev, frequency_khz, BROADCAST_ATSC3);
    if (ret != HDTVMATE_OK) return ret;

    ret = cxd6801_atsc3_set_plp_config(dev, plp_ids, plp_count);
    return ret;
}

hdtvmate_error_t cxd6801_get_stats(cxd6801_device_t *dev, signal_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

    if (dev->state == CXD6801_STATE_ACTIVE_ATSC3) {
        bool locked;
        cxd6801_atsc3_check_demod_lock(dev, &locked);
        stats->locked = locked;

        if (locked) {
            cxd6801_monitor_snr(dev, &stats->snr_db_x100);
            cxd6801_monitor_rf_level(dev, &stats->rf_level_dbm);
            cxd6801_monitor_pre_ldpc_ber(dev, &stats->ber_numerator,
                                          &stats->ber_denominator);
        }
    } else if (dev->state == CXD6801_STATE_ACTIVE_ATSC1) {
        bool locked;
        cxd6801_atsc1_check_lock(dev, &locked);
        stats->locked = locked;
    }

    return HDTVMATE_OK;
}

void cxd6801_deinit(cxd6801_device_t *dev)
{
    if (dev->initialized) {
        cxd6801_shutdown(dev);
        dev->initialized = false;
    }
}
