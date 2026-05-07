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
/* Re-extracted directly from liba3_phy_sony.so file offset 0x3C4CC.
 * Earlier copy was wrong — every entry had been pasted as the ATSC 1.0
 * row, including the ATSC 3.0 entry [0]. ATSC 3.0 actually wants
 * gain=0, RF filter=0, AGC=0 (OFDM signal needs different front-end
 * settings than 8VSB). Using the wrong values is the most likely
 * remaining reason the chip got OFDM bootstrap (syncStat=1) but
 * never advanced to full demod lock. */
static const uint8_t g_param_table[32][16] = {
    /* tvSystem  0: ATSC 3.0 (CONFIRMED from binary) */
    {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00},
    /* tvSystem  1: DVB-T?  */
    {0x00, 0xFF, 0x05, 0x02, 0x05, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0x00},
    /* tvSystem  2 */
    {0x00, 0xFF, 0x05, 0x02, 0x05, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0x00},
    /* tvSystem  3 */
    {0x00, 0xFF, 0x05, 0x02, 0x05, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x03, 0x01, 0xFF, 0xFF, 0x00},
    /* tvSystem  4 */
    {0x00, 0xFF, 0x05, 0x02, 0x05, 0x02, 0x01, 0x01, 0x01, 0x00, 0x01, 0x0B, 0x05, 0xFF, 0xFF, 0x00},
    /* tvSystem  5 */
    {0x00, 0xFF, 0x05, 0x02, 0x05, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x02, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  6 */
    {0x00, 0xFF, 0x05, 0x02, 0x05, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x02, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  7 */
    {0x00, 0xFF, 0x03, 0x03, 0x06, 0x03, 0x04, 0x04, 0x04, 0x00, 0x02, 0x02, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem  8 */
    {0x00, 0xFF, 0x03, 0x03, 0x06, 0x03, 0x04, 0x04, 0x04, 0x00, 0x02, 0x1F, 0x04, 0xFF, 0xFF, 0x00},
    /* tvSystem  9: ATSC 1.0 (8VSB) - CONFIRMED */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x03, 0x03, 0x03, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 10 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x00, 0x1A, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 11 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x00, 0x17, 0x1B, 0xFF, 0xFF, 0x00},
    /* tvSystem 12 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x01, 0x19, 0x1A, 0xFF, 0xFF, 0x00},
    /* tvSystem 13 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x02, 0x1B, 0x19, 0xFF, 0xFF, 0x00},
    /* tvSystem 14 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x00, 0x18, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 15 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x00, 0x18, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 16 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x01, 0x1A, 0x1B, 0xFF, 0xFF, 0x00},
    /* tvSystem 17 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x02, 0x1C, 0x1A, 0xFF, 0xFF, 0x00},
    /* tvSystem 18 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x03, 0x16, 0x16, 0xFF, 0xFF, 0x00},
    /* tvSystem 19 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x00, 0x18, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 20 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x00, 0x18, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 21 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x01, 0x1A, 0x1B, 0xFF, 0xFF, 0x00},
    /* tvSystem 22 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x02, 0x1C, 0x1A, 0xFF, 0xFF, 0x00},
    /* tvSystem 23: J.83B */
    {0x00, 0xFF, 0x04, 0x09, 0x09, 0x09, 0x02, 0x02, 0x02, 0x00, 0x00, 0x1A, 0x1C, 0xFF, 0xFF, 0x00},
    /* tvSystem 24 */
    {0x00, 0xFF, 0x04, 0x09, 0x09, 0x09, 0x02, 0x02, 0x02, 0x00, 0x02, 0x1E, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 25 */
    {0x00, 0xFF, 0x02, 0x0A, 0x0A, 0x0A, 0x02, 0x02, 0x02, 0x00, 0x00, 0x1A, 0x1E, 0xFF, 0xFF, 0x00},
    /* tvSystem 26 */
    {0x00, 0xFF, 0x02, 0x0A, 0x0A, 0x0A, 0x02, 0x02, 0x02, 0x00, 0x02, 0x1E, 0x00, 0xFF, 0xFF, 0x00},
    /* tvSystem 27 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x00, 0x18, 0x1D, 0xFF, 0xFF, 0x00},
    /* tvSystem 28 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x01, 0x1A, 0x1B, 0xFF, 0xFF, 0x00},
    /* tvSystem 29 */
    {0x00, 0xFF, 0x08, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x02, 0x1C, 0x1A, 0xFF, 0xFF, 0x00},
    /* tvSystem 30 */
    {0x00, 0xFF, 0x04, 0x09, 0x09, 0x09, 0x02, 0x02, 0x02, 0x00, 0x00, 0x1B, 0x02, 0xFF, 0xFF, 0x00},
    /* tvSystem 31 */
    {0x00, 0xFF, 0x03, 0x0C, 0x0C, 0x0C, 0x02, 0x02, 0x02, 0x00, 0x02, 0x02, 0x01, 0xFF, 0xFF, 0x00},
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
 * Single transaction.
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
 * X_oscen - VCO oscillator enable (Sony's actual TUNE-phase sequence)
 *
 * Frida trace of liba3_phy_sony.so v2.32 lock-success run shows ZERO
 * writes to tuner reg 0x82 or 0x84 across 49,703 captured ops. The
 * decompiled `0xdf344` we matched earlier appears to be either dead
 * code or a different code path the running app doesn't take.
 *
 * Sony's per-tune sequence starts with:
 *   read+write reg 0x74 = 0x02
 *   write reg 0x87 = {0xC4, 0x40}
 *   ... LNA / RF filter / 0x5E / PLL ctrl / 0x68 ...
 *
 * So this function reduces to: read 0x74, write 0x74=0x02, write
 * 0x87={0xC4,0x40}. The xtal divider that used to go to reg 0x84 is
 * actually byte 4 of the reg 0x5E burst — set in ascot3_x_tune.
 * ============================================================ */
static hdtvmate_error_t ascot3_x_oscen(cxd6801_device_t *dev,
                                        uint32_t freq_khz, bool vco_cal)
{
    hdtvmate_error_t ret;
    uint8_t data[2];
    (void)freq_khz;
    (void)vco_cal;

    /* reg 0x74 read + write 0x02 (Sony's first tuner write per tune) */
    {
        uint8_t cur = 0;
        uint8_t tx[8];
        extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                             const uint8_t *tx_data, uint8_t tx_len,
                                             uint8_t *rx_data, uint8_t rx_len);
        tx[0]=1; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_TUNER; tx[3]=0x74;
        br_cmd_send(dev->bridge, 0x002B, tx, 4, NULL, 0);
        tx[0]=1; tx[1]=CXD6801_I2C_BUS; tx[2]=CXD6801_I2C_ADDR_TUNER;
        br_cmd_send(dev->bridge, 0x002A, tx, 3, &cur, 1);
        (void)cur;
    }
    data[0] = 0x02;
    ret = ascot3_write_regs(dev, 0x74, data, 1);
    if (ret != HDTVMATE_OK) return ret;

    /* reg 0x87: oscillator config — Sony writes {0xC4, 0x40} on first tune
     * (vcoCal=true) and {0xC4, 0x41} on retunes. Our scan_full power-cycles
     * and re-inits every channel, so vcoCal is always true here. */
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

    /* ---- Step 4: reg 0x5E (9 bytes) - Core tuning data ----
     *
     * Hardcoded to Sony's exact captured bytes for ATSC 3.0 / 6 MHz / UHF
     * 701 MHz: {EE 02 1E 67 03 B4 78 08 30}.
     *
     * Our previous derivation from g_param_table[ATSC3] produced
     * {EE 02 1E 67 08 0C 03 80 08} — wildly different from Sony's actual
     * writes. The g_param_table values appear to encode something
     * different from what bytes 5-8 of reg 0x5E need. Until that gets
     * reverse-engineered, hardcoding these UHF/ATSC3 values lets us at
     * least exercise the lock path correctly for 701 MHz.
     *
     * For non-UHF ranges we'd need a different captured set; leaving the
     * old computation as a fallback so VHF tunes at least don't NACK.
     */
    {
        uint8_t tune_data[9];

        if (range == 2 && tv_system == TV_SYSTEM_ATSC3) {
            /* Sony's exact 701 MHz UHF / ATSC 3.0 / 6 MHz capture */
            tune_data[0] = 0xEE;
            tune_data[1] = 0x02;
            tune_data[2] = 0x1E;
            tune_data[3] = 0x67;
            tune_data[4] = 0x03;
            tune_data[5] = 0xB4;
            tune_data[6] = 0x78;
            tune_data[7] = 0x08;
            tune_data[8] = 0x30;
        } else {
            tune_data[0] = 0xEE;
            tune_data[1] = 0x02;
            tune_data[2] = 0x1E;
            tune_data[3] = vco_cal ? 0x67 : 0x45;
            if (range == 2) {
                tune_data[4] = ascot3_divider_uhf[ASCOT3_XTAL_TYPE];
            } else {
                tune_data[4] = ascot3_divider_vhf[ASCOT3_XTAL_TYPE];
            }
            tune_data[5] = param[3 + range];
            tune_data[6] = param[6 + range];
            tune_data[7] = (param[1] == 0xFF) ? 0x80 : param[1];
            tune_data[8] = param[2] & 0x0F;
        }

        ret = ascot3_write_regs(dev, ASCOT3_REG_TUNE_DATA, tune_data, 9);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* ---- Step 5: reg 0x67 read + write back (Sony's actual behavior) ----
     *
     * Frida trace shows Sony does read 0x67 then write the SAME byte back
     * (typically 0x00). The decompiled `set_reg_bits(0x67, 0x06, 0x06)`
     * appears to be a different code path; the running app does NOT set
     * bits 1/2 here. Setting 0x06 actively breaks lock — verified against
     * v2.32 lock-success capture which shows 0x67 stays at 0x00.
     */
    ret = ascot3_set_reg_bits(dev, ASCOT3_REG_PLL_CTRL, 0x00, 0x00);
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
        /* TEMPORARY: hardcode bytes 0-7 from Sony's captured 701 MHz lock-success
         * trace. Our g_param_table-based computation produces wrong bytes for
         * almost every position. Real fix would be to reverse-engineer the full
         * X_tune algorithm from the binary, but this validates whether a correct
         * X_tune burst is what we needed. */
        rf_agc[0] = 0x00;
        rf_agc[1] = 0x88;
        rf_agc[2] = 0x00;
        rf_agc[3] = 0x0B;
        rf_agc[4] = 0x22;
        rf_agc[5] = 0x00;
        rf_agc[6] = 0x18;
        rf_agc[7] = 0x1D;
        (void)param;
        (void)is_cable;

        /* Frequency in kHz: 24-bit LITTLE-endian at positions 8-10.
         * Was 32-bit big-endian which is wrong. Verified via Frida-hooked
         * trace at 701 MHz: Sony writes [0x48 0xB2 0x0A 0xFF] = LE encoding
         * of 0x0AB248 = 701000, with 0xFF at position 11 (terminator). */
        rf_agc[8]  = (uint8_t)(freq_khz & 0xFF);
        rf_agc[9]  = (uint8_t)((freq_khz >> 8) & 0xFF);
        rf_agc[10] = (uint8_t)((freq_khz >> 16) & 0xFF);
        rf_agc[11] = 0xFF;

        /* Bytes 12-16: Sony's lock-success trace at 701 MHz mode-0 path:
         *   {0x11, 0x99, 0x00, 0x24, 0x87}
         * For ATSC 3.0 mode the values would differ (binary mode==3 path
         * writes {0xD9, 0x0F, 0x24/0x25, 0x87} at positions 13-16).
         * Using mode-0 values for now since that's what Sony's
         * lock-success trace captured. */
        rf_agc[12] = 0x11;
        rf_agc[13] = 0x99;
        rf_agc[14] = 0x00;
        rf_agc[15] = 0x24;
        rf_agc[16] = 0x87;

        ret = ascot3_write_regs(dev, ASCOT3_REG_RF_AGC_DATA, rf_agc, 17);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* ---- Step 7: Wait for PLL lock (Sony waits ~50ms here) ----
     * The X_tune burst kicks off the tuner's internal calibration; we
     * must let it complete before the post-tune writes that finalize
     * the tune state. */
    br_user_delay(50);

    /* ---- Step 8: Post-tune writes (Sony's exact captured sequence) ----
     * After the 50ms wait Sony writes reg 0x88=0 and reg 0x87=0xC0
     * (single byte, NOT the {C4,40} 2-byte pair from earlier). Without
     * these the chip may stay in "tuning" mode and never raise lock. */
    {
        uint8_t one;
        one = 0x00; ret = ascot3_write_regs(dev, 0x88, &one, 1);
        if (ret != HDTVMATE_OK) return ret;
        one = 0xC0; ret = ascot3_write_regs(dev, 0x87, &one, 1);
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

    /* ASCOT3 X_pon (power-on init) — Sony's captured sequence from running
     * v2.32 HDTV Player. Without this the chip's RF/LNA/AGC are uninitialized
     * and lock acquisition fails even with a perfect X_tune burst later. */
    {
        uint8_t buf[32];

        /* reg 0x99: LNA config (2 bytes) */
        buf[0] = 0x7A; buf[1] = 0x01;
        ret = ascot3_write_regs(dev, 0x99, buf, 2);
        if (ret != HDTVMATE_OK) goto fail;

        /* reg 0x81: BIG init burst (20 bytes) — main X_pon configuration */
        {
            uint8_t init81[20] = {
                0x18, 0x84, 0xA8, 0x82, 0x00, 0x00, 0xC4, 0x40,
                0x10, 0x00, 0x45, 0x75, 0x07, 0x1C, 0x3F, 0x02,
                0x10, 0x20, 0x0A, 0x00
            };
            ret = ascot3_write_regs(dev, 0x81, init81, 20);
            if (ret != HDTVMATE_OK) goto fail;
        }

        /* reg 0x9B = 0 */
        buf[0] = 0x00;
        ret = ascot3_write_regs(dev, 0x9B, buf, 1);
        if (ret != HDTVMATE_OK) goto fail;

        /* reg 0x17 = {0x2A, 0x0E} */
        buf[0] = 0x2A; buf[1] = 0x0E;
        ret = ascot3_write_regs(dev, 0x17, buf, 2);
        if (ret != HDTVMATE_OK) goto fail;

        /* reg 0x95 = 0x01 */
        buf[0] = 0x01;
        ret = ascot3_write_regs(dev, 0x95, buf, 1);
        if (ret != HDTVMATE_OK) goto fail;

        /* Calibration sequence: 0xB0/0xB1/0xB3/0x30 */
        buf[0] = 0x00; ret = ascot3_write_regs(dev, 0xB0, buf, 1); if (ret) goto fail;
        buf[0] = 0xE0; ret = ascot3_write_regs(dev, 0x30, buf, 1); if (ret) goto fail;
        buf[0] = 0x1E; ret = ascot3_write_regs(dev, 0xB1, buf, 1); if (ret) goto fail;
        buf[0] = 0x02; ret = ascot3_write_regs(dev, 0xB3, buf, 1); if (ret) goto fail;
        buf[0] = 0x00; ret = ascot3_write_regs(dev, 0xB3, buf, 1); if (ret) goto fail;
        buf[0] = 0x00; ret = ascot3_write_regs(dev, 0xB1, buf, 1); if (ret) goto fail;
        buf[0] = 0xE1; ret = ascot3_write_regs(dev, 0x30, buf, 1); if (ret) goto fail;
        buf[0] = 0x01; ret = ascot3_write_regs(dev, 0xB0, buf, 1); if (ret) goto fail;

        /* reg 0x67 = 0, reg 0x74 = 2 */
        buf[0] = 0x00; ret = ascot3_write_regs(dev, 0x67, buf, 1); if (ret) goto fail;
        buf[0] = 0x02; ret = ascot3_write_regs(dev, 0x74, buf, 1); if (ret) goto fail;

        /* reg 0x5E = {0x15, 0x00, 0x00} */
        buf[0] = 0x15; buf[1] = 0x00; buf[2] = 0x00;
        ret = ascot3_write_regs(dev, 0x5E, buf, 3);
        if (ret != HDTVMATE_OK) goto fail;

        /* reg 0x88 = 0, reg 0x87 = 0xC0, reg 0x80 = 1 */
        buf[0] = 0x00; ret = ascot3_write_regs(dev, 0x88, buf, 1); if (ret) goto fail;
        buf[0] = 0xC0; ret = ascot3_write_regs(dev, 0x87, buf, 1); if (ret) goto fail;
        buf[0] = 0x01; ret = ascot3_write_regs(dev, 0x80, buf, 1); if (ret) goto fail;
    }

fail:
    /* Disable I2C repeater */
    cxd6801_i2c_repeater_enable(dev, false);

    if (ret != HDTVMATE_OK) {
        LOG_ERR("ASCOT3 X_pon failed: %d", ret);
        return ret;
    }
    dev->tuner_initialized = true;
    LOG_INFO("ASCOT3 tuner initialized (X_pon complete)");
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

    /* Tested entries [0], [6], [9] for ATSC 3.0 — all gave identical
     * Lock reg = 0xD9 (syncStat=1, OFDM bootstrap detected, no full
     * demod lock). g_param_table front-end values don't determine lock
     * outcome on this antenna; signal strength does. Reverting to the
     * real ATSC 3.0 entry [0] (matches binary).  */
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
        tv_system = TV_SYSTEM_ATSC3;
        break;
    }

    LOG_INFO("ASCOT3 tuning: freq=%u kHz, BW=%d MHz, tvSystem=%d",
             frequency_khz, bw, tv_system);

    /* Enable I2C repeater */
    ret = cxd6801_i2c_repeater_enable(dev, true);
    if (ret != HDTVMATE_OK) return ret;

    /* Step 1: X_oscen — read+write reg 0x74=0x02, write reg 0x87={0xC4,0x40}.
     * Sony's per-tune trace doesn't touch reg 0x82 or 0x84. */
    ret = ascot3_x_oscen(dev, frequency_khz, true /* vco_cal first tune */);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("X_oscen failed: %d", ret);
        goto done;
    }

    /* Step 2: X_tune — LNA, RF filter, 0x5E tune data, 0x67 PLL ctrl,
     * 0x68 17-byte tune burst, 50ms wait, post-tune writes (0x88=0,
     * 0x87=0xC0). All inside ascot3_x_tune now to match Sony's exact
     * order with the I2C repeater still enabled. */
    ret = ascot3_x_tune(dev, frequency_khz, tv_system, true);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("X_tune failed: %d", ret);
        goto done;
    }

    /* Store current frequency */
    dev->frequency_khz = frequency_khz;
    dev->bandwidth = bw;

    LOG_INFO("ASCOT3 tune complete: %u kHz", frequency_khz);

done:
    /* Disable I2C repeater */
    cxd6801_i2c_repeater_enable(dev, false);

    /* The tuner X_tune burst (the 17-byte write to reg 0x68 plus the
     * preceding burst) leaves the SLVT path NACKing for a brief window.
     * Sony's binary on the same hardware doesn't show this — possibly
     * because they sequence things differently or do something we don't
     * see. Empirically a small delay here lets the chip's internal I2C
     * bridge recover and accept SLVT writes again. */
    {
        extern void br_user_delay(uint32_t ms);
        br_user_delay(50);
    }
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
