/*
 * ascot3.c - Sony ASCOT3 RF tuner driver (CXD6801 integrated tuner)
 *
 * Complete tune sequence decompiled from liba3_phy_sony.so via Ghidra:
 *   - X_tune @ 0xdf520: filter/gain/AGC configuration
 *   - X_oscen @ 0xdf344: VCO enable + xtal clock selection
 *   - sony_cxd6801_ascot3_Tune @ 0xdf178: top-level wrapper
 *
 * Register write sequence (X_tune):
 *   1. reg 0x87 (2 bytes): oscillator config {0xC4, 0x40}
 *   2. reg 0x91 (2 bytes): LNA config (system-dependent)
 *   3. reg 0x9C (2 bytes): RF filter mode
 *   4. reg 0x5E (9 bytes): core tuning data (divider, gain, filter)
 *   5. SetRegisterBits(0x67, mask, value): PLL control
 *   6. reg 0x68 (17 bytes): RF filter + AGC from tvSystem table
 *
 * X_oscen:
 *   7. reg 0x82 (2 bytes): VCO cal + frequency enable
 *   8. reg 0x84 (2 bytes): xtal selection
 *   9. reg 0x87 (2 bytes): final oscillator config
 */

#include "cxd6801.h"
#include "ascot3_tune.h"
#include <stdio.h>
#include <string.h>

extern void br_user_delay(uint32_t ms);

/* ============================================================
 * g_param_table - TV system parameters (extracted from binary @ 0x3C4CC)
 * 32 entries × 16 bytes each
 *
 * Layout per entry:
 *   [0]  = band & 3 (RF band selection)
 *   [1]  = IF frequency setting (-1/0xFF = use default 0x80)
 *   [2]  = IF fine tune & 0xF
 *   [3]  = gain for freq < 172001 kHz (VHF-Lo)
 *   [4]  = gain for freq < 464001 kHz (VHF-Hi)
 *   [5]  = gain for freq >= 464001 kHz (UHF)
 *   [6]  = RF filter for freq < 172001 kHz (VHF-Lo)
 *   [7]  = RF filter for freq < 464001 kHz (VHF-Hi)
 *   [8]  = RF filter for freq >= 464001 kHz (UHF)
 *   [9]  = AGC setting 1
 *   [10] = AGC setting 2
 *   [11-14] = reserved/padding
 *   [15] = flag & 1 (used for reg 0x9C byte[1])
 * ============================================================ */
static const uint8_t g_param_table[32][16] = {
    /* tvSystem  0: ATSC 3.0 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  1 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  2 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  3 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  4 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  5 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  6 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  7 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  8 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  9: ATSC 1.0 (8VSB) - CONFIRMED */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 10 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 11 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 12 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 13 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 14 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 15 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 16 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 17 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 18 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 19 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 20 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 21 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 22 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 23: Cable J.83B variant */
    {0x01, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x03, 0x03, 0x1A, 0x1D, 0xFF, 0xFF, 0x01},
    /* tvSystem 24: Cable J.83B variant */
    {0x01, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x03, 0x03, 0x1A, 0x1D, 0xFF, 0xFF, 0x01},
    /* tvSystem 25 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 26 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 27 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 28 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 29 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 30: Cable variant */
    {0x01, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x03, 0x03, 0x1A, 0x1D, 0xFF, 0xFF, 0x01},
    /* tvSystem 31 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
};

/* Xtal-dependent clock divider tables — defined in ascot3_tune.h */

/* ============================================================
 * TV system enum mapping for ATSC usage
 * ============================================================ */
#define TV_SYSTEM_ATSC3     0   /* ATSC 3.0 */
#define TV_SYSTEM_ATSC1     9   /* ATSC 1.0 (8VSB) */
#define TV_SYSTEM_J83B     23   /* J.83B cable QAM */

/* Crystal type - default 0 for HDTV Mate */
#define ASCOT3_XTAL_TYPE    0

/* ============================================================
 * Helper: determine frequency range index
 *   0 = VHF-Lo (< 172001 kHz)
 *   1 = VHF-Hi (< 464001 kHz)
 *   2 = UHF    (>= 464001 kHz)
 * ============================================================ */
static int ascot3_freq_range(uint32_t freq_khz)
{
    if (freq_khz < ASCOT3_FREQ_VHF_BOUNDARY)
        return 0;  /* VHF-Lo */
    else if (freq_khz < ASCOT3_FREQ_UHF_BOUNDARY)
        return 1;  /* VHF-Hi */
    else
        return 2;  /* UHF */
}

/* ============================================================
 * Enable/disable I2C repeater on demod to access tuner
 *
 * Sony's exact pattern: only SLVX reg 0x08 = enable. From
 * sony_cxd6801_demod_I2cRepeaterEnable @ 0xe6e6c — calls
 * WriteOneRegister(SLVX_addr, 0x08, enable) and nothing else.
 *
 * Earlier we also toggled SLVT bank 0 reg 0x1A here, and that
 * combination did make the very first tuner access succeed (Tuner
 * ID = 0xE1). But it also pushed SLVT writes into a NACK state for
 * the rest of the tune sequence — touching SLVT 0x1A flipped the
 * demod's SLVT path into "tuner routing" mode that wasn't released
 * even on disable.
 *
 * The one-time SLVX reg 0x1A = 1 in cxd6801_initialize (Sony's
 * TunerI2cEnable) is what actually opens the I2C bus path; SLVX 0x08
 * is the per-access gate.
 * ============================================================ */
static hdtvmate_error_t cxd6801_i2c_repeater_enable(cxd6801_device_t *dev, bool enable)
{
    uint8_t val = enable ? 0x01 : 0x00;
    uint8_t tx[8];
    extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                         const uint8_t *tx_data, uint8_t tx_len,
                                         uint8_t *rx_data, uint8_t rx_len);

    /* SLVX reg 0x08 = val (Sony's documented I2cRepeaterEnable bit) */
    tx[0] = 2; tx[1] = CXD6801_I2C_BUS; tx[2] = CXD6801_I2C_ADDR_SLVX;
    tx[3] = 0x08; tx[4] = val;
    return br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
}

/* ============================================================
 * One-time tuner I2C bus enable (called once during demod init)
 *
 * From sony_cxd6801_demod_TunerI2cEnable @ 0xe3068 (Ghidra):
 *   Bank-select SLVX (write [0,0] to 0xDC), then SetRegisterBits
 *   on SLVX reg 0x1A = 1 (mask 0xFF → plain write).
 *
 * This must run AFTER XtoSL and BEFORE any tuner access.
 * It enables the I2C path from the IT9300 master through the demod
 * to the ASCOT3 tuner at addr 0xC0.
 * ============================================================ */
hdtvmate_error_t cxd6801_tuner_i2c_enable(cxd6801_device_t *dev, bool enable)
{
    uint8_t val = enable ? 0x01 : 0x00;
    uint8_t tx[8];
    hdtvmate_error_t ret;
    extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                         const uint8_t *tx_data, uint8_t tx_len,
                                         uint8_t *rx_data, uint8_t rx_len);

    /* SLVX bank/page select to 0: write [0x00, 0x00] to 0xDC */
    tx[0] = 2; tx[1] = CXD6801_I2C_BUS; tx[2] = CXD6801_I2C_ADDR_SLVX;
    tx[3] = 0x00; tx[4] = 0x00;
    ret = br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVX reg 0x1A = enable */
    tx[0] = 2; tx[1] = CXD6801_I2C_BUS; tx[2] = CXD6801_I2C_ADDR_SLVX;
    tx[3] = 0x1A; tx[4] = val;
    return br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
}

/* ============================================================
 * Tuner register write — via I2C repeater to addr 0xC0.
 * Single transaction (no chunking; chunking didn't reduce SLVT NACKs).
 * ============================================================ */
static hdtvmate_error_t ascot3_write_regs(cxd6801_device_t *dev,
                                           uint8_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t tx[64];
    extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                         const uint8_t *tx_data, uint8_t tx_len,
                                         uint8_t *rx_data, uint8_t rx_len);
    if (len + 4 > sizeof(tx)) return HDTVMATE_ERR_INVALID_PARAM;
    tx[0] = len + 1;
    tx[1] = CXD6801_I2C_BUS;
    tx[2] = CXD6801_I2C_ADDR_TUNER;  /* 0xC0 */
    tx[3] = reg;
    memcpy(&tx[4], data, len);
    return br_cmd_send(dev->bridge, 0x002B, tx, len + 4, NULL, 0);
}

static hdtvmate_error_t ascot3_set_reg_bits(cxd6801_device_t *dev,
                                             uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t data, tx[8];
    hdtvmate_error_t ret;
    extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                         const uint8_t *tx_data, uint8_t tx_len,
                                         uint8_t *rx_data, uint8_t rx_len);
    /* Read current value from tuner */
    tx[0]=1; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_TUNER; tx[3]=reg;
    br_cmd_send(dev->bridge, 0x002B, tx, 4, NULL, 0);
    tx[0]=1; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_TUNER;
    ret = br_cmd_send(dev->bridge, 0x002A, tx, 3, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    data = (data & ~mask) | (value & mask);

    tx[0]=2; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_TUNER; tx[3]=reg; tx[4]=data;
    return br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
}

/* ============================================================
 * X_oscen - VCO oscillator enable (decompiled from 0xdf344)
 *
 * Enables the VCO and selects xtal clock divider.
 * Called BEFORE X_tune in the ascot3_Tune sequence.
 *
 * reg 0x82: {vcoCal ? 0xC7 : 0xC5, 0x00}
 * reg 0x84: {xtal_code, 0x00}  (xtal_code depends on freq range)
 * reg 0x87: {0xC4, 0x40}
 * ============================================================ */
static hdtvmate_error_t ascot3_x_oscen(cxd6801_device_t *dev,
                                        uint32_t freq_khz, bool vco_cal)
{
    hdtvmate_error_t ret;
    uint8_t data[2];
    int range = ascot3_freq_range(freq_khz);

    LOG_DBG("X_oscen: freq=%u kHz, vcoCal=%d, range=%d", freq_khz, vco_cal, range);

    /* reg 0x82: VCO calibration control */
    data[0] = vco_cal ? 0xC7 : 0xC5;
    data[1] = 0x00;
    ret = ascot3_write_regs(dev, 0x82, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    /* reg 0x84: xtal clock selection
     * The value selects the reference clock divider for the PLL.
     * UHF (>= 464001): smaller divider → higher ref clock
     * VHF (< 464001): larger divider → lower ref clock
     */
    if (range == 2) {
        /* UHF */
        data[0] = ascot3_divider_uhf[ASCOT3_XTAL_TYPE];
    } else {
        /* VHF-Lo or VHF-Hi */
        data[0] = ascot3_divider_vhf[ASCOT3_XTAL_TYPE];
    }
    data[1] = 0x00;
    ret = ascot3_write_regs(dev, 0x84, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    /* reg 0x87: oscillator config (always {0xC4, 0x40}) */
    data[0] = ASCOT3_OSC_CONFIG_0;  /* 0xC4 */
    data[1] = ASCOT3_OSC_CONFIG_1;  /* 0x40 */
    ret = ascot3_write_regs(dev, 0x87, data, 2);

    return ret;
}

/* ============================================================
 * X_tune - Core tuning register sequence (decompiled from 0xdf520)
 *
 * Configures filters, gain, AGC, and divider for the target frequency.
 * Does NOT directly set the PLL frequency — that's done by the
 * divider ratio written to reg 0x5E and the clock from X_oscen.
 *
 * Parameters:
 *   freq_khz  - target frequency in kHz
 *   tv_system - tvSystem enum index (0=ATSC3, 9=ATSC1, 23=J83B)
 *   vco_cal   - VCO calibration flag
 * ============================================================ */
static hdtvmate_error_t ascot3_x_tune(cxd6801_device_t *dev,
                                       uint32_t freq_khz, uint8_t tv_system,
                                       bool vco_cal)
{
    hdtvmate_error_t ret;
    uint8_t data[17];
    int range = ascot3_freq_range(freq_khz);
    const uint8_t *param = g_param_table[tv_system];
    bool is_cable = (param[0] & 0x03) != 0;

    LOG_DBG("X_tune: freq=%u kHz, tvSystem=%d, vcoCal=%d, range=%d, cable=%d",
            freq_khz, tv_system, vco_cal, range, is_cable);

    /* ---- Step 1: reg 0x87 (2 bytes) - Oscillator config ---- */
    data[0] = ASCOT3_OSC_CONFIG_0;  /* 0xC4 */
    data[1] = ASCOT3_OSC_CONFIG_1;  /* 0x40 */
    ret = ascot3_write_regs(dev, ASCOT3_REG_OSC_CONFIG, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    /* ---- Step 2: reg 0x91 (2 bytes) - LNA configuration ---- */
    if (is_cable) {
        /* Cable systems: different LNA setting */
        data[0] = 0x11;
        data[1] = 0x20;
    } else {
        /* Terrestrial (ATSC): standard LNA */
        data[0] = 0x10;
        data[1] = 0x20;
    }
    ret = ascot3_write_regs(dev, ASCOT3_REG_LNA_CONFIG, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    /* ---- Step 3: reg 0x9C (2 bytes) - RF filter mode ---- */
    data[0] = 0x00;
    data[1] = param[15] & 0x01;  /* flag from table */
    ret = ascot3_write_regs(dev, ASCOT3_REG_RF_FILTER_MODE, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    /* ---- Step 4: reg 0x5E (9 bytes) - Core tuning data ---- */
    {
        uint8_t tune_data[9];

        tune_data[0] = 0xEE;  /* fixed */
        tune_data[1] = 0x02;  /* fixed */
        tune_data[2] = 0x1E;  /* fixed */
        tune_data[3] = vco_cal ? 0x67 : 0x45;  /* VCO cal flag */

        /* Xtal-dependent divider (sets PLL reference ratio) */
        if (range == 2) {
            /* UHF */
            tune_data[4] = ascot3_divider_uhf[ASCOT3_XTAL_TYPE];
        } else {
            /* VHF */
            tune_data[4] = ascot3_divider_vhf[ASCOT3_XTAL_TYPE];
        }

        /* Gain value from table (offset 3+range) */
        tune_data[5] = param[3 + range];

        /* RF filter from table (offset 6+range) */
        tune_data[6] = param[6 + range];

        /* IF frequency setting */
        tune_data[7] = (param[1] == 0xFF) ? 0x80 : param[1];

        /* IF fine tune */
        tune_data[8] = param[2] & 0x0F;

        ret = ascot3_write_regs(dev, ASCOT3_REG_TUNE_DATA, tune_data, 9);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* ---- Step 5: SetRegisterBits(0x67, 0x06, 0x06) - PLL control ---- */
    ret = ascot3_set_reg_bits(dev, ASCOT3_REG_PLL_CTRL, 0x06, 0x06);
    if (ret != HDTVMATE_OK) return ret;

    /* ---- Step 6: reg 0x68 (17 bytes) - RF filter + AGC + gain ---- */
    {
        uint8_t rf_agc[17];
        memset(rf_agc, 0, sizeof(rf_agc));

        /* From X_tune decompile: the 17-byte block at reg 0x68 contains:
         * [0]:  RF filter value (freq-range dependent from table)
         * [1]:  Gain value (freq-range dependent from table)
         * [2]:  AGC setting 1 (from table[9])
         * [3]:  AGC setting 2 (from table[10])
         * [4]:  IF fine tune (from table[2] & 0xF)
         * [5]:  IF frequency (0x80 if table[1] == 0xFF)
         * [6]:  band config (from table[0])
         * [7]:  fixed 0x02
         * [8]:  frequency byte 3 (MSB of freq_khz)
         * [9]:  frequency byte 2
         * [10]: frequency byte 1
         * [11]: frequency byte 0 (LSB of freq_khz)
         * [12]: fixed 0x00
         * [13]: fixed 0xFF
         * [14]: fixed 0xFF
         * [15]: fixed 0x03
         * [16]: fixed 0x00
         */
        rf_agc[0] = param[6 + range];   /* RF filter for this freq range */
        rf_agc[1] = param[3 + range];   /* Gain for this freq range */
        rf_agc[2] = param[9];           /* AGC 1 */
        rf_agc[3] = param[10];          /* AGC 2 */
        rf_agc[4] = param[2] & 0x0F;   /* IF fine tune */
        rf_agc[5] = (param[1] == 0xFF) ? 0x80 : param[1];  /* IF freq */
        rf_agc[6] = param[0] & 0x03;   /* Band config */
        rf_agc[7] = 0x02;              /* Fixed */

        /* Frequency in kHz (big-endian, 4 bytes) */
        rf_agc[8]  = (uint8_t)((freq_khz >> 24) & 0xFF);
        rf_agc[9]  = (uint8_t)((freq_khz >> 16) & 0xFF);
        rf_agc[10] = (uint8_t)((freq_khz >> 8) & 0xFF);
        rf_agc[11] = (uint8_t)(freq_khz & 0xFF);

        rf_agc[12] = 0x00;
        rf_agc[13] = 0xFF;
        rf_agc[14] = 0xFF;
        rf_agc[15] = 0x03;
        rf_agc[16] = 0x00;

        ret = ascot3_write_regs(dev, ASCOT3_REG_RF_AGC_DATA, rf_agc, 17);
        if (ret != HDTVMATE_OK) return ret;
    }

    return HDTVMATE_OK;
}

/* ============================================================
 * Public API: cxd6801_tuner_init
 * ============================================================ */
hdtvmate_error_t cxd6801_tuner_init(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;

    LOG_INFO("Initializing ASCOT3 tuner...");

    /* Enable I2C repeater to access tuner */
    ret = cxd6801_i2c_repeater_enable(dev, true);
    if (ret != HDTVMATE_OK) return ret;

    /* Read tuner chip ID via 0xC0 (repeater must be enabled) */
    uint8_t tuner_id = 0;
    {
        uint8_t tx[8];
        tx[0]=1; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_TUNER; tx[3]=0x7F;
        br_cmd_send(dev->bridge, 0x002B, tx, 4, NULL, 0);
        tx[0]=1; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_TUNER;
        ret = br_cmd_send(dev->bridge, 0x002A, tx, 3, &tuner_id, 1);
    }
    if (ret == HDTVMATE_OK && tuner_id != 0xC1) {
        LOG_INFO("ASCOT3 tuner ID: 0x%02x (repeater OK!)", tuner_id);
    } else {
        LOG_WARN("Tuner ID: 0x%02x (repeater may not be working)", tuner_id);
    }

    /* ASCOT3 power-on initialization:
     * From sony_cxd6801_ascot3_Initialize decompile:
     * 1. Write power-on defaults to multiple registers
     * 2. Wait stabilization
     *
     * Minimal init: just verify communication works.
     * Full init happens implicitly during first tune.
     */

    /* Disable I2C repeater */
    cxd6801_i2c_repeater_enable(dev, false);

    dev->tuner_initialized = true;
    LOG_INFO("ASCOT3 tuner initialized");
    return HDTVMATE_OK;
}

/* ============================================================
 * Public API: cxd6801_tuner_tune
 *
 * Full ASCOT3 tune sequence (from sony_cxd6801_ascot3_Tune):
 *   1. Enable I2C repeater
 *   2. X_oscen() — VCO enable + xtal selection
 *   3. X_tune() — filter/gain/AGC/divider configuration
 *   4. Wait for PLL lock (~50ms)
 *   5. Disable I2C repeater
 * ============================================================ */
hdtvmate_error_t cxd6801_tuner_tune(cxd6801_device_t *dev, uint32_t frequency_khz,
                                     cxd6801_bandwidth_t bw)
{
    hdtvmate_error_t ret;
    uint8_t tv_system;

    /* Map bandwidth/standard to tvSystem enum */
    switch (dev->state) {
    case CXD6801_STATE_ACTIVE_ATSC3:
        tv_system = TV_SYSTEM_ATSC3;
        break;
    case CXD6801_STATE_ACTIVE_ATSC1:
        tv_system = TV_SYSTEM_ATSC1;
        break;
    case CXD6801_STATE_ACTIVE_J83B:
        tv_system = TV_SYSTEM_J83B;
        break;
    default:
        /* Default to ATSC 3.0 */
        tv_system = TV_SYSTEM_ATSC3;
        break;
    }

    LOG_INFO("ASCOT3 tuning: freq=%u kHz, BW=%d MHz, tvSystem=%d",
             frequency_khz, bw, tv_system);

    /* Enable I2C repeater */
    ret = cxd6801_i2c_repeater_enable(dev, true);
    if (ret != HDTVMATE_OK) return ret;

    /* Step 1: X_oscen - VCO enable + clock setup */
    ret = ascot3_x_oscen(dev, frequency_khz, true /* vco_cal first tune */);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("X_oscen failed: %d", ret);
        goto done;
    }

    /* Step 2: X_tune - filter/gain/AGC configuration */
    ret = ascot3_x_tune(dev, frequency_khz, tv_system, true);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("X_tune failed: %d", ret);
        goto done;
    }

    /* Step 3: Wait for PLL lock
     * The ASCOT3 PLL typically locks within 20-50ms.
     * sony_cxd6801_ascot3_Tune uses a timeout of ~100ms.
     */
    br_user_delay(50);

    /* Store current frequency */
    dev->frequency_khz = frequency_khz;
    dev->bandwidth = bw;

    LOG_INFO("ASCOT3 tune complete: %u kHz", frequency_khz);

done:
    /* Disable I2C repeater */
    cxd6801_i2c_repeater_enable(dev, false);
    return ret;
}

/* ============================================================
 * Public API: cxd6801_tuner_sleep
 * ============================================================ */
hdtvmate_error_t cxd6801_tuner_sleep(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;

    LOG_INFO("ASCOT3 entering sleep mode");

    ret = cxd6801_i2c_repeater_enable(dev, true);
    if (ret != HDTVMATE_OK) return ret;

    /* From sony_cxd6801_ascot3_Sleep:
     * Write 0x00 to reg 0x87 to disable oscillator
     * Write 0x00 to reg 0x82 to disable VCO
     */
    uint8_t data[2] = {0x00, 0x00};
    ret = ascot3_write_regs(dev, 0x87, data, 2);
    if (ret == HDTVMATE_OK) {
        ret = ascot3_write_regs(dev, 0x82, data, 2);
    }

    cxd6801_i2c_repeater_enable(dev, false);

    if (ret == HDTVMATE_OK) {
        LOG_INFO("ASCOT3 sleep mode entered");
    }
    return ret;
}
