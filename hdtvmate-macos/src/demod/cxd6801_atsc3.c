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

/* Placeholder register sequences - TO BE FILLED FROM GHIDRA */
/* These will be populated after decompiling sony_cxd6801_demod_atsc3_Tune */
static const reg_value_t atsc3_tune_common[] = {
    /* Common setup for ATSC 3.0 mode transition
     * TODO: Extract from Ghidra decompilation
     * Expected ~20-50 register writes */
    {0x00, 0x2C, 0x01},  /* Placeholder: system mode = ATSC 3.0 */
    {0, 0, 0},  /* Terminator */
};

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

    /* Step 2: Apply common ATSC 3.0 tune register sequence
     * sony_cxd6801_demod_atsc3_Tune() does:
     * - Mode transition from SLEEP to ACTIVE_ATSC3
     * - Configure IFAGC, bandwidth filter, clock
     * - Set up ALP output
     */
    for (int i = 0; atsc3_tune_common[i].bank || atsc3_tune_common[i].reg; i++) {
        const reg_value_t *rv = &atsc3_tune_common[i];
        ret = cxd6801_i2c_write_one(&dev->i2c_demod, rv->bank, rv->reg, rv->value);
        if (ret != HDTVMATE_OK) {
            LOG_ERR("Register write failed at tune step %d", i);
            return ret;
        }
    }

    /* Step 3: Tune the ASCOT3 tuner to the target frequency */
    ret = cxd6801_tuner_tune(dev, frequency_khz, bw);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Tuner tune failed");
        return ret;
    }

    /* Step 4: Tune end (sony_cxd6801_demod_TuneEnd / sony_cxd6801_ascot3_TuneEnd)
     * Release from reset, start acquisition */
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
    /* Release demod from tuning hold and start acquisition.
     * From sony_cxd6801_demod_TuneEnd():
     * - Soft reset
     * - Enable TS/ALP output
     */
    hdtvmate_error_t ret;

    /* Soft reset to start acquisition */
    ret = cxd6801_soft_reset(dev);
    if (ret != HDTVMATE_OK) return ret;

    br_user_delay(50);  /* Wait for acquisition to start */

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_check_demod_lock(cxd6801_device_t *dev, bool *locked)
{
    uint8_t data = 0;
    hdtvmate_error_t ret;

    *locked = false;

    /*
     * Check demodulator lock status.
     * From sony_cxd6801_demod_atsc3_CheckDemodLock():
     * Read lock status register (bank 0x91, reg TBD)
     *
     * Lock status bits (from CXD2880 reference):
     * bit 0: TPS lock
     * bit 1: Demod lock
     * bit 2: TS lock
     *
     * TODO: Confirm exact bank/register from Ghidra
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x10, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    /* Check demod lock bit (bit pattern TBD) */
    *locked = (data & 0x07) == 0x07;

    LOG_TRC("Demod lock status: 0x%02x -> %s", data, *locked ? "LOCKED" : "unlocked");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_check_alp_lock(cxd6801_device_t *dev, bool *locked)
{
    uint8_t data = 0;
    hdtvmate_error_t ret;

    *locked = false;

    /*
     * Check ALP lock (ATSC 3.0 transport layer lock).
     * From sony_cxd6801_demod_atsc3_CheckALPLock():
     * This confirms the ALP framing is synchronized.
     *
     * TODO: Confirm exact bank/register from Ghidra
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x11, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    *locked = (data & 0x01) != 0;

    LOG_TRC("ALP lock status: 0x%02x -> %s", data, *locked ? "LOCKED" : "unlocked");
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
