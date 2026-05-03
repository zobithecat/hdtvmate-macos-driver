#include "cxd6801.h"
#include <stdio.h>
#include <string.h>

/*
 * cxd6801_atsc3.c - ATSC 3.0 tuning, lock detection, PLP configuration
 *
 * Key functions ported from liba3_phy_sony.so symbols:
 * - sony_cxd6801_demod_atsc3_Tune
 * - sony_cxd6801_demod_atsc3_CheckDemodLock
 * - sony_cxd6801_demod_atsc3_CheckALPLock
 * - sony_cxd6801_demod_atsc3_SetPLPConfig
 * - sony_cxd6801_integ_atsc3_Tune
 * - sony_cxd6801_integ_atsc3_WaitALPLock
 * - sony_cxd6801_integ_atsc3_Scan
 *
 * Register map for ATSC 3.0 mode (derived from CXD2880 DVB-T2 patterns):
 * - Bank 0x00: Common control
 * - Bank 0x90: ATSC 3.0 demod control (tune, bandwidth)
 * - Bank 0x91: ATSC 3.0 lock status
 * - Bank 0x92: ATSC 3.0 L1 signaling
 * - Bank 0x93: ATSC 3.0 PLP configuration
 *
 * TODO: Exact bank/register numbers must be confirmed via Ghidra analysis
 * of the binary. Current values are educated guesses from CXD2880 patterns.
 */

extern void br_user_delay(uint32_t ms);
extern uint64_t br_user_time_ms(void);

/*
 * ATSC 3.0 bandwidth configuration registers
 * Based on sony_cxd6801_demod_atsc3_Tune() decompilation structure
 */
typedef struct {
    uint8_t bank;
    uint8_t reg;
    uint8_t value;
} reg_value_t;

/*
 * SLtoAA3 register sequence - Sleep to Active ATSC 3.0 transition
 * Decompiled from liba3_phy_sony.so SLtoAA3() at 0xeaafc via Ghidra.
 *
 * Note: The binary uses separate I2C addresses for different sub-blocks:
 *   - SLVT (i2cAddressSLVT): Main demod registers (our 0xDC)
 *   - SLVX (i2cAddressSLVX): Extended registers
 *   The "bank" is set by writing to reg 0x00 before accessing registers.
 *   In our driver, the bank is the first parameter to cxd6801_i2c_write_one().
 *
 * ALP output mode (ATSC 3.0 normal, non-EAS):
 */

/*
 * SLtoAA3 - Sleep to Active ATSC 3.0 mode transition
 * Applies the full register initialization sequence from Ghidra decompile.
 */
static hdtvmate_error_t cxd6801_sltoaa3(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;

    LOG_DBG("SLtoAA3: applying mode transition registers...");

    /* SLVX bank 0x00: reg 0x17 = 0x0E (enable ATSC3 clock) */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x17, 0x0E);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x00: output mode = ALP (0x02) */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xA9, 0x02);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x00: system select = ATSC 3.0 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x2C, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x00: reg 0x4B = 0x74 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x4B, 0x74);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x00: reg 0x49 = 0x00 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x49, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVX bank 0x00: reg 0x18 = 0x00 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x18, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x11: reg 0x6A = 0x50 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x11, 0x6A, 0x50);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x11: reg 0x33 = {0x00, 0x03, 0x3B} */
    {
        uint8_t data[3] = {0x00, 0x03, 0x3B};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x11, 0x33, data, 3);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* SLVT bank 0x95: reg 0x79 = 0x10 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x95, 0x79, 0x10);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x95: reg 0x7B = 0x10 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x95, 0x7B, 0x10);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x9C: reg 0x50 (5 bytes) - Normal EAS state */
    {
        uint8_t data[5] = {0x93, 0x40, 0x09, 0xC0, 0x00};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9C, 0x50, data, 5);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* SLVT bank 0x9C: reg 0x65 (5 bytes) */
    {
        uint8_t data[5] = {0xD3, 0x40, 0x00, 0x1C, 0x1D};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9C, 0x65, data, 5);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* SLVT bank 0x9C: reg 0xD4 (3 bytes) */
    {
        uint8_t data[3] = {0x01, 0xD8, 0x1D};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9C, 0xD4, data, 3);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* SLVT bank 0x9C: reg 0xE0 (3 bytes) */
    {
        uint8_t data[3] = {0x01, 0xD8, 0x1D};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9C, 0xE0, data, 3);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* SLVT bank 0x9C: reg 0xFC = 0x14 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9C, 0xFC, 0x14);
    if (ret != HDTVMATE_OK) return ret;

    /*
     * SLtoAA3_BandSetting() for 6 MHz (ATSC 3.0)
     * Decompiled from Ghidra at 0xed7f0, BW==6 path.
     * Sets nominal rate, ITB coefficients, and filter config.
     */

    /* Bank 0x90: reg 0x9F = nominalRate (5 bytes)
     * For 6 MHz: 0x711CC71B + 0xC7 (little-endian → {1B,C7,1C,71,C7}) */
    {
        uint8_t nominalRate[5] = {0x1B, 0xC7, 0x1C, 0x71, 0xC7};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x90, 0x9F, nominalRate, 5);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Bank 0x10: reg 0xA5 = 0x00 (no IQ inversion) */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x10, 0xA5, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* Bank 0x10: reg 0xA6 = ITB coefficients (14 bytes)
     * Extracted from binary .rodata at offset 0x3cbf8 */
    {
        uint8_t itbCoef[14] = {
            0x31, 0xA8, 0x29, 0x9B, 0x27, 0x9C, 0x28,
            0x9E, 0x29, 0xA4, 0x29, 0xA2, 0x29, 0xA8
        };
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x10, 0xA6, itbCoef, 14);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Bank 0x10: reg 0xD7 = 0x04 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x10, 0xD7, 0x04);
    if (ret != HDTVMATE_OK) return ret;

    /* Bank 0x1D: reg 0xBF = 10 bytes (filter config)
     * From binary .rodata at offset 0x3cc06 */
    {
        uint8_t filterData[10] = {
            0x01, 0x1E, 0xC3, 0x3E, 0xC2, 0x79, 0x84, 0x1E, 0xC3, 0x3E
        };
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x1D, 0xBF, filterData, 10);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Bank 0x99: reg 0x89 = 4 bytes (0xE40D39DE LE → {DE,39,0D,E4}) */
    {
        uint8_t data4[4] = {0xDE, 0x39, 0x0D, 0xE4};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x99, 0x89, data4, 4);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Enable stream output: bank 0x02, reg 0xC0 = 0x00 (enable ALP output) */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x02, 0xC0, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    LOG_DBG("SLtoAA3: mode transition + band setting complete");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_tune(cxd6801_device_t *dev, uint32_t frequency_khz,
                                     cxd6801_bandwidth_t bw)
{
    hdtvmate_error_t ret;

    LOG_INFO("ATSC 3.0 tune: %u kHz, BW=%d MHz", frequency_khz, bw);

    if (!dev->initialized) {
        return HDTVMATE_ERR_DEMOD_INIT;
    }

    /* Step 1: Ensure sleep state */
    if (dev->state == CXD6801_STATE_ACTIVE_ATSC3 ||
        dev->state == CXD6801_STATE_ACTIVE_ATSC1) {
        ret = cxd6801_sleep(dev);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Step 2: SLtoAA3 - Sleep to Active ATSC 3.0 mode transition
     * From Ghidra decompile of SLtoAA3() at 0xeaafc */
    ret = cxd6801_sltoaa3(dev);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("SLtoAA3 mode transition failed");
        return ret;
    }

    /* Step 3: Tune the ASCOT3 tuner to the target frequency */
    ret = cxd6801_tuner_tune(dev, frequency_khz, bw);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Tuner tune failed");
        return ret;
    }

    /* Step 4: TuneEnd - SoftReset + SetStreamOutput to start acquisition
     * From sony_cxd6801_demod_TuneEnd(): SoftReset + SetStreamOutput */
    ret = cxd6801_atsc3_tune_end(dev);
    if (ret != HDTVMATE_OK) return ret;

    dev->state = CXD6801_STATE_ACTIVE_ATSC3;
    dev->frequency_khz = frequency_khz;
    dev->bandwidth = bw;

    LOG_INFO("ATSC 3.0 tune started, waiting for lock...");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_tune_end(cxd6801_device_t *dev)
{
    /*
     * From Ghidra decompile of sony_cxd6801_demod_TuneEnd():
     * For non-ATSC1: SoftReset() then SetStreamOutput()
     *
     * SoftReset: bank 0x00, reg 0xFE = 0x01
     * SetStreamOutput: enables TS/ALP output pin
     */
    hdtvmate_error_t ret;

    /* Soft reset to start acquisition (bank 0x00, reg 0xFE = 0x01) */
    ret = cxd6801_soft_reset(dev);
    if (ret != HDTVMATE_OK) return ret;

    /* SetStreamOutput - enable ALP output
     * From Ghidra: This writes to output enable register.
     * The exact register depends on output mode but is typically
     * bank 0x00, reg 0xC3 for stream output enable.
     * For now, the SoftReset alone should trigger acquisition. */

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_check_demod_lock(cxd6801_device_t *dev, bool *locked)
{
    uint8_t data = 0;
    hdtvmate_error_t ret;

    *locked = false;

    /*
     * Check demodulator lock status.
     * From Ghidra decompile of sony_cxd6801_demod_atsc3_CheckDemodLock():
     *   1. Call monitor_SyncStat() which reads:
     *      - Bank 0x90, reg 0x10: syncStat = data & 0x07, unlockDetected = (data>>4)&1
     *      - Bank 0x95, reg 0x40: ALP lock bits
     *   2. If syncStat > 5 → locked (value 6 = demod lock achieved)
     *   3. If unlockDetected → unlock detected
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x90, 0x10, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    uint8_t sync_stat = data & 0x07;
    uint8_t unlock_detected = (data >> 4) & 0x01;

    if (unlock_detected) {
        LOG_TRC("Demod unlock detected (reg=0x%02x)", data);
        *locked = false;
    } else {
        *locked = (sync_stat > 5);  /* syncStat 6 = locked */
    }

    LOG_TRC("Demod lock: bank=0x90 reg=0x10 = 0x%02x, sync=%d, unlock=%d -> %s",
            data, sync_stat, unlock_detected, *locked ? "LOCKED" : "unlocked");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_check_alp_lock(cxd6801_device_t *dev, bool *locked)
{
    uint8_t data = 0;
    hdtvmate_error_t ret;

    *locked = false;

    /*
     * Check ALP lock (ATSC 3.0 transport layer lock).
     * From Ghidra decompile of monitor_SyncStat():
     *   Bank 0x95, reg 0x40:
     *     bit 4: ALPLockAll (all PLPs locked)
     *     bit 0: alpLockStat[0]
     *     bit 1: alpLockStat[1]
     *     bit 2: alpLockStat[2]
     *     bit 3: alpLockStat[3]
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x95, 0x40, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    *locked = ((data >> 4) & 0x01) != 0;  /* ALPLockAll bit */

    LOG_TRC("ALP lock: bank=0x95 reg=0x40 = 0x%02x -> %s",
            data, *locked ? "LOCKED" : "unlocked");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_wait_lock(cxd6801_device_t *dev, uint32_t timeout_ms)
{
    /*
     * Wait for both demod lock and ALP lock.
     * From sony_cxd6801_integ_atsc3_WaitALPLock():
     * 1. Poll demod lock status
     * 2. Once demod is locked, poll ALP lock
     * 3. Timeout if neither locks within timeout_ms
     */
    uint64_t start = br_user_time_ms();
    bool demod_locked = false;
    bool alp_locked = false;

    LOG_DBG("Waiting for ATSC 3.0 lock (timeout=%u ms)...", timeout_ms);

    while ((br_user_time_ms() - start) < timeout_ms) {
        /* Check demod lock first */
        if (!demod_locked) {
            cxd6801_atsc3_check_demod_lock(dev, &demod_locked);
            if (demod_locked) {
                LOG_INFO("Demod locked at %llu ms",
                         (unsigned long long)(br_user_time_ms() - start));
            }
        }

        /* Once demod is locked, check ALP lock */
        if (demod_locked) {
            cxd6801_atsc3_check_alp_lock(dev, &alp_locked);
            if (alp_locked) {
                LOG_INFO("ALP locked at %llu ms - channel acquired!",
                         (unsigned long long)(br_user_time_ms() - start));
                return HDTVMATE_OK;
            }
        }

        br_user_delay(50);  /* Poll interval */
    }

    if (!demod_locked) {
        LOG_WARN("Demod lock timeout at %u kHz", dev->frequency_khz);
    } else {
        LOG_WARN("ALP lock timeout at %u kHz (demod was locked)", dev->frequency_khz);
    }

    return HDTVMATE_ERR_NO_LOCK;
}

hdtvmate_error_t cxd6801_atsc3_set_plp_config(cxd6801_device_t *dev,
                                                const uint8_t *plp_ids, uint8_t count)
{
    hdtvmate_error_t ret;

    LOG_INFO("Setting PLP config: %d PLPs", count);

    if (count > 64) count = 64;

    /*
     * Configure PLP selection.
     * From sony_cxd6801_demod_atsc3_SetPLPConfig() / DRV_CXD6801_setPlpConfig():
     * Write PLP IDs to the demodulator for filtering.
     *
     * TODO: Extract exact register sequence from Ghidra
     * Expected registers: bank 0x93, PLP count + PLP IDs
     */

    /* Write PLP count */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x93, 0x10, count);
    if (ret != HDTVMATE_OK) return ret;

    /* Write PLP IDs */
    if (count > 0) {
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x93, 0x11, plp_ids, count);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Update local config */
    dev->plp_config.num_plps = count;
    memcpy(dev->plp_config.plp_ids, plp_ids, count);

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_sleep(cxd6801_device_t *dev)
{
    /*
     * Transition from ACTIVE_ATSC3 to SLEEP.
     * From sony_cxd6801_demod_atsc3_Sleep():
     * - Disable ALP output
     * - Set sleep mode register
     *
     * TODO: Extract from Ghidra
     */
    hdtvmate_error_t ret;

    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x2C, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    ret = cxd6801_tuner_sleep(dev);
    if (ret != HDTVMATE_OK) return ret;

    dev->state = CXD6801_STATE_SLEEP;
    return HDTVMATE_OK;
}

/* --- ATSC 1.0 operations --- */

hdtvmate_error_t cxd6801_atsc1_tune(cxd6801_device_t *dev, uint32_t frequency_khz)
{
    hdtvmate_error_t ret;

    LOG_INFO("ATSC 1.0 tune: %u kHz", frequency_khz);

    if (dev->state != CXD6801_STATE_SLEEP) {
        ret = cxd6801_sleep(dev);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* TODO: ATSC 1.0 tune register sequence from Ghidra
     * sony_cxd6801_demod_atsc_Tune()
     * sony_cxd6801_integ_atsc_Tune()
     */

    ret = cxd6801_tuner_tune(dev, frequency_khz, CXD6801_BW_6MHZ);
    if (ret != HDTVMATE_OK) return ret;

    ret = cxd6801_atsc3_tune_end(dev);  /* Similar tune-end sequence */
    if (ret != HDTVMATE_OK) return ret;

    dev->state = CXD6801_STATE_ACTIVE_ATSC1;
    dev->frequency_khz = frequency_khz;

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc1_check_lock(cxd6801_device_t *dev, bool *locked)
{
    uint8_t data = 0;
    hdtvmate_error_t ret;

    *locked = false;

    /* TODO: ATSC 1.0 lock check register from Ghidra
     * sony_cxd6801_integ_atsc_WaitTSLock()
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x20, 0x10, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    *locked = (data & 0x07) == 0x07;
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc1_wait_lock(cxd6801_device_t *dev, uint32_t timeout_ms)
{
    uint64_t start = br_user_time_ms();
    bool locked = false;

    while ((br_user_time_ms() - start) < timeout_ms) {
        cxd6801_atsc1_check_lock(dev, &locked);
        if (locked) {
            LOG_INFO("ATSC 1.0 locked at %llu ms",
                     (unsigned long long)(br_user_time_ms() - start));
            return HDTVMATE_OK;
        }
        br_user_delay(50);
    }

    LOG_WARN("ATSC 1.0 lock timeout at %u kHz", dev->frequency_khz);
    return HDTVMATE_ERR_NO_LOCK;
}
