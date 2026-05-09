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
extern hdtvmate_error_t br_cmd_write_registers(it9300_device_t *dev,
                                                uint8_t processor,
                                                uint32_t addr,
                                                const uint8_t *values, uint8_t len);
extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);

/* Direct SLVX (0xDC) single-register write — flat address space, no bank.
 * Sony's SLtoAA3 has two SLVX writes that we were incorrectly routing to
 * SLVT before the i2c wrapper would silently turn them into bank-selects.
 * SLVX has its own slave address; we must send the I2C write manually
 * via the IT9300 bridge command 0x002B. */
static hdtvmate_error_t cxd6801_slvx_write_one(cxd6801_device_t *dev,
                                                 uint8_t reg, uint8_t value)
{
    uint8_t tx[5];
    tx[0] = 2;                          /* register address byte count */
    tx[1] = dev->i2c_demod.i2c_bus;     /* I2C bus index */
    tx[2] = CXD6801_I2C_ADDR_SLVX;      /* 0xDC */
    tx[3] = reg;
    tx[4] = value;
    return br_cmd_send(dev->bridge, 0x002B, tx, 5, NULL, 0);
}

/* Hardware power-cycle + reset the demod chip via IT9300 GPIOs.
 *
 * Found by ADB logcat capture of HDTV Player on LineageOS UTM:
 *   SonyPHYAndroid::tune (mode 5 = ATSC 3.0) is preceded by
 *     _setPwrEn[0]  → D8E3 = 0 (chip Vdd off)
 *     _setPwrEn[1]  → D8E3 = 1 (chip Vdd on)
 *
 * Combine with D8B7 (demod reset) toggle so the chip starts in a
 * fully-known state, like the very first call to it9300_initialize. */
static hdtvmate_error_t cxd6801_chip_power_cycle(it9300_device_t *bridge)
{
    uint8_t val;
    LOG_INFO("Chip power cycle + reset: D8E3 + D8B7");

    /* Power off */
    val = 0x00; br_cmd_write_registers(bridge, 1, 0xD8E3, &val, 1);
    /* Hold reset asserted while power is off */
    val = 0x00; br_cmd_write_registers(bridge, 1, 0xD8B7, &val, 1);
    br_user_delay(500);

    /* Power on */
    val = 0x01; br_cmd_write_registers(bridge, 1, 0xD8E3, &val, 1);
    br_user_delay(200);
    /* Release reset */
    val = 0x01; br_cmd_write_registers(bridge, 1, 0xD8B7, &val, 1);
    br_user_delay(500);

    return HDTVMATE_OK;
}

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

/* sony_cxd6801_demod_atsc3_AutoDetectSeq_Init @ 0xed4ac
 *
 * Run after SoftReset and before SLtoAA3, while the demod is still
 * in SLEEP (state == 2). Configures the bank-0x90 ATSC 3.0 auto-
 * detect path and the CW-detection sub-block.
 *
 * Register sequence (all SLVT, bank-relative writes use our
 * cxd6801_i2c_write_one helper which selects the bank first):
 *   bank 0x90:
 *     reg 0xF3 = 0     // detect-running flag
 *     reg 0x9A = 0     // CW detection: clear
 *     reg 0x38 = 4     // (CW-related)
 *     reg 0x9B = 0
 *     reg 0x11 = 0x20
 *     reg 0x9A = 0     // re-clear
 *     reg 0x3C = {05,05,00,00,00,00,00,00}  // 8 bytes
 *     reg 0x50 = 5
 */
static hdtvmate_error_t cxd6801_atsc3_auto_detect_seq_init(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;

    LOG_DBG("AutoDetectSeq_Init: bank 0x90/0x9A/0x9B setup");

    /* AutoDetectSeq_Init: bank 0x90 reg 0xF3 = 0 (detect-running flag) */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x90, 0xF3, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* initCWDetection — Sony's actual sequence (verified via Frida-hooked
     * trace of the running app). The ARM disassembly was misread earlier:
     * `mov w3, #0x9A` followed by reg=0 means BANK-SELECT to 0x9A, NOT a
     * write to register 0x9A. So initCWDetection ops live in banks 0x9A
     * and 0x9B, NOT bank 0x90 as we had them. */

    /* Bank 0x9A: reg 0x38 = 0x04 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9A, 0x38, 0x04);
    if (ret != HDTVMATE_OK) return ret;

    /* Bank 0x9B: reg 0x11 = 0x20 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9B, 0x11, 0x20);
    if (ret != HDTVMATE_OK) return ret;

    /* Bank 0x9A: reg 0x3C = 8 bytes {05,05,00,00,00,00,00,00} */
    {
        uint8_t cw_data[8] = { 0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9A, 0x3C, cw_data, 8);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Bank 0x9A: reg 0x50 = 0x05 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9A, 0x50, 0x05);
    return ret;
}

/* sony_cxd6801_demod_atsc3_SetPLPConfig @ 0xec054
 *
 * Configures which PLPs (Physical Layer Pipes) the demod will track.
 * Each PLP byte gets bit 0x80 set (presumably "valid" flag).
 *
 * For HDTV Mate's typical case (1 PLP, ID 0): plp_ids = [0x80, 0, 0, 0]
 *   bank 0x93:
 *     reg 0x80 = [plp0|0x80, plp1|0x80 or 0, plp2..., plp3...]  (4 bytes)
 *     reg 0x85 = (demod->[0x2a6] != 0) ? 1 : 0   // we use 0 (default)
 *     reg 0x9C = 1
 */
static hdtvmate_error_t cxd6801_atsc3_set_plp_config_internal(cxd6801_device_t *dev,
                                                                uint8_t num_plps,
                                                                const uint8_t *plp_ids)
{
    hdtvmate_error_t ret;
    uint8_t buf[4] = {0, 0, 0, 0};

    if (num_plps > 4) num_plps = 4;
    for (uint8_t i = 0; i < num_plps && i < 4; i++) {
        buf[i] = plp_ids[i] | 0x80;
    }

    LOG_DBG("SetPLPConfig: %d PLPs, buf={%02x %02x %02x %02x}",
            num_plps, buf[0], buf[1], buf[2], buf[3]);

    ret = cxd6801_i2c_write(&dev->i2c_demod, 0x93, 0x80, buf, 4);
    if (ret != HDTVMATE_OK) return ret;
    /* Sony writes 0x01 here (verified via Frida-hooked trace of running app);
     * comes from device->[0x2a6] which is set during init, not 0 as we had. */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x93, 0x85, 0x01);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x93, 0x9C, 0x01);
    return ret;
}

/*
 * SLtoAA3 - Sleep to Active ATSC 3.0 mode transition
 * Applies the full register initialization sequence from Ghidra decompile.
 */
static hdtvmate_error_t cxd6801_sltoaa3(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;

    LOG_DBG("SLtoAA3: applying mode transition (Sony v2.32 lock-success sequence)...");

    /* === Sony's exact sequence captured via Frida hook on v2.32 LOCK SUCCESS ===
     * Source: spawn-mode capture, lock register reached 0x06 (sync_stat=6 LOCKED).
     * This sequence is taken verbatim from the running app's I2C trace. */

    /* (1) Bank 0x00 reg 0xC4: Korean mode bit 7 set first (0x29 → 0xA9). */
    ret = cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0xC4, 0x80, 0x80);
    if (ret != HDTVMATE_OK) return ret;

    /* (2) Bank 0x02 reg 0xE4 = 0x00 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x02, 0xE4, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* (3) Bank 0x00 reg 0xC4: clear bit 3 (0xA9 → 0xA1) */
    ret = cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0xC4, 0x08, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* (4) AutoDetectSeq_Init / initCWDetection — bank 0x90 + 0x9A + 0x9B */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x90, 0xF3, 0x00);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9A, 0x38, 0x04);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9B, 0x11, 0x20);
    if (ret != HDTVMATE_OK) return ret;
    {
        uint8_t cw_data[8] = { 0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9A, 0x3C, cw_data, 8);
        if (ret != HDTVMATE_OK) return ret;
    }
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9A, 0x50, 0x05);
    if (ret != HDTVMATE_OK) return ret;

    /* (5) PLP config — bank 0x93 */
    {
        uint8_t plp_data[4] = {0x80, 0x00, 0x00, 0x00};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x93, 0x80, plp_data, 4);
        if (ret != HDTVMATE_OK) return ret;
    }
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x93, 0x85, 0x01);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x93, 0x9C, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* (6) Bank 0x00 init writes — D3, DE, DA, C4 (re-confirm), D1, D9 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xD3, 0x00);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xDE, 0x01);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xDA, 0x01);
    if (ret != HDTVMATE_OK) return ret;
    /* re-confirm C4 = 0xA1 (Sony writes again here) */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xC4, 0xA1);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xD1, 0x01);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xD9, 0x08);
    if (ret != HDTVMATE_OK) return ret;

    /* (7) Clock setup: reg 0x32=0, 0x33=1, 0x32=1 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x32, 0x00);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x33, 0x01);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x32, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* (8) Bank 0x01 reg 0xE7 = 0x00 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x01, 0xE7, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* (9) Bank 0x10 reg 0x66 = 0x01 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x10, 0x66, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* (10) Bank 0x40 reg 0x66 = 0x01 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x40, 0x66, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* (11) Bank 0x95 reg 0x11 = 0x5C (ALP clock frequency — was 0x60 wrong) */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x95, 0x11, 0x5C);
    if (ret != HDTVMATE_OK) return ret;

    /* (12) Bank 0x02 reg 0xE7 = 0x01 */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x02, 0xE7, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* (13) SLVX (0xDC) reg 0x17 = 0x0E (was 0x0F wrong, lock-success trace shows 0x0E) */
    ret = cxd6801_slvx_write_one(dev, 0x17, 0x0E);
    if (ret != HDTVMATE_OK) return ret;

    /* (14) Bank 0x00 mode/system select */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xA9, 0x00);  /* output = TS */
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x2C, 0x01);  /* system = ATSC3 */
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x4B, 0x74);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x49, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* (15) SLVX (0xDC) reg 0x18 = 0x00 */
    ret = cxd6801_slvx_write_one(dev, 0x18, 0x00);
    if (ret != HDTVMATE_OK) return ret;

    /* (16) Bank 0x11 control */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x11, 0x6A, 0x50);
    if (ret != HDTVMATE_OK) return ret;
    {
        uint8_t data[3] = {0x00, 0x03, 0x3B};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x11, 0x33, data, 3);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* (17) Bank 0x95 reg 0x79, 0x7B */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x95, 0x79, 0x10);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x95, 0x7B, 0x10);
    if (ret != HDTVMATE_OK) return ret;

    /* SLVT bank 0x9C: Normal-mode (non-EAS) configuration block.
     * Re-extracted byte-by-byte from binary @ 0xeb1d8..0xeb314.
     * Previous values had 3 corrupted bytes that broke acquisition. */

    /* reg 0x50 (5 bytes): w8=0xC0094093 LE → {93,40,09,C0}, then sturb #1 → 0x01 */
    {
        uint8_t data[5] = {0x93, 0x40, 0x09, 0xC0, 0x01};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9C, 0x50, data, 5);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* reg 0x65 (5 bytes): w8=0x40D3 → {D3,40,00,00}, then strb wzr → 0x00 */
    {
        uint8_t data[5] = {0xD3, 0x40, 0x00, 0x00, 0x00};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9C, 0x65, data, 5);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* reg 0xD4 (3 bytes): strh #0xD801 → {01,D8}, strb #0x1C → 0x1C */
    {
        uint8_t data[3] = {0x01, 0xD8, 0x1C};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9C, 0xD4, data, 3);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* reg 0xE0 (3 bytes): strh #0xD801 → {01,D8}, strb #0x1D → 0x1D */
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

    /* Bank 0x10: reg 0xA6 = ITB coefficients (14 bytes)
     * Extracted from binary .rodata at offset 0x3cbf8.
     * Sony writes A6 BEFORE A5 — verified via Frida-hooked trace. */
    {
        uint8_t itbCoef[14] = {
            0x31, 0xA8, 0x29, 0x9B, 0x27, 0x9C, 0x28,
            0x9E, 0x29, 0xA4, 0x29, 0xA2, 0x29, 0xA8
        };
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x10, 0xA6, itbCoef, 14);
        if (ret != HDTVMATE_OK) return ret;
    }

    /* Bank 0x10: reg 0xA5 = 0x01.
     * Was 0x00 in our code (mis-read as "no IQ inversion"). Frida trace
     * shows Sony writes 0x01 here — comes from device->[0x280] flag. */
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x10, 0xA5, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* Bank 0x10: reg 0xB6 = 3 bytes {0x13, 0x33, 0x33} — IF freq config.
     * This was the "opaque IF config table" we couldn't extract from the
     * binary (loaded at runtime via SetIFFreqConfig from device->[0x25c..0x25e]).
     * Frida trace of the running app captured the actual bytes. */
    {
        uint8_t ifFreqCfg[3] = {0x13, 0x33, 0x33};
        ret = cxd6801_i2c_write(&dev->i2c_demod, 0x10, 0xB6, ifFreqCfg, 3);
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

    /* Final SetRegisterBits operations on bank 0x00 (from lock-success trace):
     *   reg 0x80: read 0x3F → write 0x28 (clear bits 0x1F, set 0x08) */
    ret = cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0x80, 0x1F, 0x08);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("SLtoAA3 SetRegisterBits at SLVT[0x80] failed");
        return ret;
    }

    /*   reg 0x81: clear bit 0 (write 0xFE) — observed in lock-success trace */
    ret = cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0x81, 0x01, 0x00);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("SLtoAA3 SetRegisterBits at SLVT[0x81] failed");
        return ret;
    }

    /* NOTE: bank 0x00 reg 0xC3 = 0x00 used to be written here, but Sony's
     * lock-success trace puts it AFTER the SoftReset in TuneEnd. Moved
     * to cxd6801_atsc3_tune_end() to match the captured order. Writing it
     * pre-SoftReset stomped on the stream-output state the chip needed
     * during acquisition. */

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

    /* Step 0: Power-cycle the chip and re-initialize.
     * Sony's app does this on every ATSC 3.0 (mode 5) tune. */
    cxd6801_chip_power_cycle(dev->bridge);
    {
        extern hdtvmate_error_t cxd6801_read_chip_id(cxd6801_device_t *dev);
        cxd6801_read_chip_id(dev);
        dev->initialized = false;
        dev->tuner_initialized = false;
        ret = cxd6801_initialize(dev);
        if (ret != HDTVMATE_OK) {
            LOG_ERR("Re-init after power cycle failed");
            return ret;
        }
    }

    /* Step 1: SoftReset before SLtoAA3 (Sony's acquireChannel does this) */
    ret = cxd6801_soft_reset(dev);
    if (ret != HDTVMATE_OK) {
        LOG_WARN("SoftReset at tune start failed (continuing)");
    }

    /* Step 1.5: AutoDetectSeq_Init + SetPLPConfig.
     * Sony's integ_atsc3_Tune @ 0x105d90 calls these BEFORE
     * demod_atsc3_Tune (= SLtoAA3). The auto-detect sequence configures
     * bank 0x90 acquisition pipe; PLP config selects which PLP(s) the
     * demod tracks. Without these, the chip may pick wrong defaults
     * for ATSC 3.0 acquisition.
     *
     * Both must run while demod is still in SLEEP — they use SLVT
     * writes which work fine here, but get NACK-locked after X_tune. */
    ret = cxd6801_atsc3_auto_detect_seq_init(dev);
    if (ret != HDTVMATE_OK) {
        LOG_WARN("AutoDetectSeq_Init failed (continuing)");
    }

    /* SetPLPConfig — honor caller-supplied dev->plp_config if populated
     * (set num_plps > 0 + plp_ids[] before calling tune to override).
     * Otherwise default to single PLP id=0. Korean broadcasts sometimes
     * carry the content stream on PLP 1 or 2 (PLP 0 = signaling). */
    {
        uint8_t plp_buf[4] = { 0 };
        uint8_t plp_n = 1;
        if (dev->plp_config.num_plps > 0 && dev->plp_config.num_plps <= 4) {
            plp_n = dev->plp_config.num_plps;
            memcpy(plp_buf, dev->plp_config.plp_ids, plp_n);
            LOG_INFO("Tune: using caller-supplied PLP config (n=%d, plp[0]=%d)",
                     plp_n, plp_buf[0]);
        }
        ret = cxd6801_atsc3_set_plp_config_internal(dev, plp_n, plp_buf);
        if (ret != HDTVMATE_OK) {
            LOG_WARN("SetPLPConfig failed (continuing)");
        }
    }

    /* Tested: SetConfig case 0x00/0x01/0x02 all map to bank 0 reg 0xC4
     * bits 7/3/4 — neither polarity changed Lock reg (still 0xD9).
     * Conclusion: KOREAN_MODE in Sony's binary is most likely a
     * software-only flag (logcat printout) without actual chip
     * register effect, OR it lives in a register we haven't found.
     * Default reg 0xC4 = 0x29 confirms chip already has bits 0/3/5
     * set, so app's "Korean OFF" default isn't gating anything we
     * can see. */

    /* Step 2: SLtoAA3 - Sleep to Active ATSC 3.0 mode transition */
    ret = cxd6801_sltoaa3(dev);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("SLtoAA3 mode transition failed");
        return ret;
    }

    /* Step 3: Tune the ASCOT3 tuner */
    ret = cxd6801_tuner_tune(dev, frequency_khz, bw);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Tuner tune failed");
        return ret;
    }

    br_user_delay(10);

    /* REMOVED: 2nd SLtoAA3 + tuner_tune. The X_tune burst in tuner_tune
     * leaves the SLVT path in a state where subsequent SLVT writes NACK
     * (verified via -vv trace: 184 NACKs after first tuner_tune). Sony's
     * actual flow is a SINGLE pass: SLtoAA3 → tuner_tune → TuneEnd.
     * Then the SoftReset inside TuneEnd is what the chip needs. */

    /* Step 4: TuneEnd - SoftReset to trigger acquisition sequence */
    ret = cxd6801_atsc3_tune_end(dev);
    if (ret != HDTVMATE_OK) return ret;

    /* REMOVED: Step 5 BandSetting re-apply.
     * Sony's lock-success trace shows NO BandSetting writes after
     * SoftReset — only one read of reg 0xA9, write reg 0xC3=0x00, then
     * lock check loop. Our re-apply was generating extra NACKs and
     * possibly stomping on chip state. */

    dev->state = CXD6801_STATE_ACTIVE_ATSC3;
    dev->frequency_khz = frequency_khz;
    dev->bandwidth = bw;

    LOG_INFO("ATSC 3.0 tune started, waiting for lock...");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc3_tune_end(cxd6801_device_t *dev)
{
    /*
     * From Frida-captured v2.32 lock-success trace, post-tuner sequence:
     *   bank 0x00 select
     *   write fe = 0x01   (SoftReset)
     *   bank 0x00 select
     *   read  a9 = 0x00   (purpose unclear; likely an internal sync poke)
     *   write c3 = 0x00   (SetStreamOutput)
     *
     * Order matters — moving c3 here (it used to live at the end of
     * SLtoAA3) lets the chip's acquisition state machine come up clean
     * after SoftReset rather than fighting an early stream-output write.
     */
    hdtvmate_error_t ret;

    /* SoftReset: bank 0x00 reg 0xFE = 0x01 */
    ret = cxd6801_soft_reset(dev);
    if (ret != HDTVMATE_OK) return ret;

    /* SetStreamOutput sequence — verified byte-by-byte from Sony's
     * libusb_bulk_transfer payload trace (sony_payload.log):
     *   1. bank 0x00 select        (i2c_write reg 0x00 = 0x00)
     *   2. read reg 0xA9 (1 byte)  — value discarded; appears to be a
     *      read-trigger or sync-poke that some chip state-machine bit
     *      gates on. Sony always reads this between SoftReset and the
     *      C3 write. We previously dropped it because it caused NACKs
     *      in wait_lock polling, but those NACKs were due to the
     *      0xF424 wrap direction being inverted (now fixed).
     *   3. bank 0x00 select again  (Sony does this again — we omit
     *      since cxd6801_i2c_write_one handles bank internally)
     *   4. write reg 0xC3 = 0x00   (the actual SetStreamOutput action) */
    {
        uint8_t a9_val = 0;
        (void)cxd6801_i2c_read(&dev->i2c_demod, 0x00, 0xA9, &a9_val, 1);
        LOG_DBG("Sony-pattern reg 0xA9 read: 0x%02x", a9_val);
    }

    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xC3, 0x00);
    return ret;
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
     *   Bank 0x95, reg 0x40:
     *     bit 4: ALPLockAll (all PLPs locked)
     *     bits[3:0]: alpLockStat[0..3] per-PLP lock
     *
     * Frida trace of Sony's running app on this hardware shows reg 0x40
     * read 3632 times with data=0x00 in 3631 of them and data=0x95 only
     * once — so ALPLockAll is genuinely rare even for a working stack.
     * Accept ANY per-PLP bit set so we don't block data capture when a
     * single PLP is decoding fine.
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x95, 0x40, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    *locked = (data & 0x1F) != 0;  /* any PLP bit OR ALPLockAll */

    LOG_TRC("ALP lock: bank=0x95 reg=0x40 = 0x%02x -> %s",
            data, *locked ? "LOCKED" : "unlocked");
    return HDTVMATE_OK;
}

/*
 * sony_cxd6801_demod_atsc3_AutoDetectSeq_SetCWTracking @ 0xed05c
 *
 * Reads continuous-wave / pilot tracking status from bank 0x9A reg 0x46
 * (10 bytes). When valid CW info is present, computes a frequency-offset
 * correction value and writes it back to bank 0x9A regs 0x3E (3 bytes),
 * 0x3C, and 0xC6. Without this active correction, the demod's
 * auto-detect state machine stays parked at sync_stat=1 (bootstrap
 * detected, frame sync never acquired).
 *
 * Returns HDTVMATE_OK and sets *done = 0 when CW tracking is complete,
 * or *done = 1 when still in progress (caller should poll again).
 *
 * The math (cw_v2 - 0x4000) * scale / bw is fixed-point arithmetic
 * extracted directly from the Sony binary at offsets ed360..ed378.
 */
static hdtvmate_error_t cxd6801_atsc3_set_cw_tracking(cxd6801_device_t *dev,
                                                      uint8_t *done)
{
    hdtvmate_error_t ret;
    uint8_t cw_buf[10];
    uint8_t signal_type_byte;
    uint32_t scale_factor, bw_divisor;

    if (!dev || !done) return HDTVMATE_ERR_INVALID_PARAM;

    /* Read 10-byte CW tracking status from bank 0x9A reg 0x46 */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x9A, 0x46, cw_buf, 10);
    if (ret != HDTVMATE_OK) return ret;

    /* Bit 0 of byte 0 == 0 means CW tracking already complete. */
    if (!(cw_buf[0] & 0x01)) {
        *done = 0;
        return HDTVMATE_OK;
    }

    /* Parse CW info (big-endian-ish packed fields).
     * cw_v0 = buf[1..4] big-endian   (32-bit signal magnitude?)
     * cw_v1 = bits from buf[5..7]    (signal threshold metric)
     * cw_v2 = bits from buf[8..9]    (CW frequency offset, 15-bit signed) */
    uint32_t cw_v0 = ((uint32_t)cw_buf[1] << 24) | ((uint32_t)cw_buf[2] << 16)
                   | ((uint32_t)cw_buf[3] << 8)  | cw_buf[4];
    uint32_t cw_v1 = (((uint32_t)cw_buf[5] & 0x01) << 16)
                   | ((uint32_t)cw_buf[6] << 8) | cw_buf[7];
    uint32_t cw_v2 = (((uint32_t)cw_buf[8] & 0x7F) << 8) | cw_buf[9];

    /* Sanity check 1: cw_v1 must be ≥ 10001 */
    if (cw_v1 < 0x2711) {
        *done = 1;
        return HDTVMATE_OK;
    }

    /* Sanity check 2: cw_v1 must exceed cw_v0 / 200 (fixed-point scaled).
     * Sony uses (cw_v0 * 0x51EB851F) >> 38, equivalent to cw_v0 / 200. */
    uint32_t threshold = (uint32_t)(((uint64_t)cw_v0 * 0x51EB851FULL) >> 38);
    if (cw_v1 <= threshold) {
        *done = 1;
        return HDTVMATE_OK;
    }

    /* Read signal-type byte from bank 0x90 reg 0x5C (top 3 bits classify
     * the OFDM signal type; selects the scale factor for the offset calc). */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x90, 0x5C, &signal_type_byte, 1);
    if (ret != HDTVMATE_OK) return ret;

    uint8_t st = signal_type_byte >> 5;
    if (st == 1)      scale_factor = 0xC0;  /* 192 */
    else if (st == 4) scale_factor = 0x60;  /* 96 */
    else if (st == 5) scale_factor = 0x30;  /* 48 */
    else {
        /* Unknown signal type — Sony returns error 4 here */
        *done = 1;
        return HDTVMATE_OK;
    }

    /* Bandwidth divisor: 6/7/8 MHz. We always tune at 6 MHz for ATSC 3.0. */
    bw_divisor = 6;

    /* Compute correction: signed (cw_v2 - 0x4000) scaled and divided */
    int32_t cw_v2_signed = (int32_t)cw_v2 - 0x4000;
    int32_t offset = (cw_v2_signed * (int32_t)scale_factor)
                   / (int32_t)bw_divisor;
    uint32_t offset_u = (uint32_t)offset;

    /* Pack into 3-byte big-endian buffer. Sony only keeps low 2 bits of MSB. */
    uint8_t off_buf[3];
    off_buf[0] = (uint8_t)((offset_u >> 16) & 0x03);
    off_buf[1] = (uint8_t)((offset_u >> 8) & 0xFF);
    off_buf[2] = (uint8_t)(offset_u & 0xFF);

    /* Write the correction values to bank 0x9A */
    ret = cxd6801_i2c_write(&dev->i2c_demod, 0x9A, 0x3E, off_buf, 3);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9A, 0x3C, 0x04);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_write_one(&dev->i2c_demod, 0x9A, 0xC6, 0x0C);
    if (ret != HDTVMATE_OK) return ret;

    *done = 1;  /* Correction written; tracking still in progress */
    return HDTVMATE_OK;
}

/*
 * atsc3_WaitCWTracking @ 0x1062dc
 *
 * Polls SetCWTracking every 10 ms for up to 300 ms. Each poll reads
 * status, and (when status is valid) writes a refined frequency-offset
 * correction to the chip. Returns HDTVMATE_OK once *done becomes 0.
 */
hdtvmate_error_t cxd6801_atsc3_wait_cw_tracking(cxd6801_device_t *dev)
{
    const uint32_t timeout_ms = 300;
    uint64_t start = br_user_time_ms();
    int iterations = 0;

    LOG_DBG("WaitCWTracking: polling (timeout=%u ms)", timeout_ms);

    while ((br_user_time_ms() - start) < timeout_ms) {
        uint8_t done = 1;
        hdtvmate_error_t ret = cxd6801_atsc3_set_cw_tracking(dev, &done);
        if (ret != HDTVMATE_OK) {
            LOG_WARN("SetCWTracking error %d at iter %d (continuing)",
                     ret, iterations);
        } else if (done == 0) {
            LOG_INFO("CW tracking acquired after %d iterations (%llu ms)",
                     iterations,
                     (unsigned long long)(br_user_time_ms() - start));
            return HDTVMATE_OK;
        }
        iterations++;
        br_user_delay(10);
    }

    LOG_WARN("WaitCWTracking timeout after %d iterations", iterations);
    return HDTVMATE_ERR_NO_LOCK;
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

    /* Sony's flow: WaitDemodLock (poll until sync_stat≥6) → WaitCWTracking
     * → WaitALPLock. WaitCWTracking only runs AFTER demod is locked —
     * empirically bank 0x9A returns NACK on bank-select before lock,
     * suggesting the chip gates that bank behind partial lock. */

    bool cw_tracked = false;

    while ((br_user_time_ms() - start) < timeout_ms) {
        /* Check demod lock first */
        if (!demod_locked) {
            cxd6801_atsc3_check_demod_lock(dev, &demod_locked);
            if (demod_locked) {
                LOG_INFO("Demod locked at %llu ms",
                         (unsigned long long)(br_user_time_ms() - start));
            }
        }

        /* Once demod is locked, run CW tracking (only legal after lock —
         * bank 0x9A returns NACK before lock). Then check ALP lock. */
        if (demod_locked && !cw_tracked) {
            hdtvmate_error_t cw_ret = cxd6801_atsc3_wait_cw_tracking(dev);
            if (cw_ret != HDTVMATE_OK) {
                LOG_DBG("WaitCWTracking did not fully converge — proceeding to ALP");
            }
            cw_tracked = true;  /* run only once per lock attempt */
        }

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
        return HDTVMATE_ERR_NO_LOCK;
    }
    /* Demod locked but ALP didn't — return success anyway. Sony's running
     * app sees ALP_lock_all clear ~99.97% of the time even on working
     * streams, so requiring it would prevent capture from ever starting.
     * Demod lock is sufficient for the IT9300 to forward TS/ALP data on
     * EP 0x84; downstream parsing can deal with ALP-level framing. */
    LOG_INFO("Demod locked at %u kHz; ALP timer expired but capture is OK", dev->frequency_khz);
    return HDTVMATE_OK;
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

/* SLtoAA1 - Sleep to Active ATSC 1.0 mode transition.
 *
 * Decompiled from sony_cxd6801_demod_atsc_Tune → SLtoAA @ 0xe93b4.
 * Key differences from SLtoAA3 (ATSC 3.0):
 *   - SetTSClockModeAndFreq(5)   instead of SetALPClockModeAndFreq(6)
 *   - SLVX reg 0x17 = 0x0F        (vs 0x0E for ATSC3)
 *   - SLVT reg 0xA9 = 0x00 (TS)   (vs 0x02 ALP for ATSC3)
 *   - SLVT reg 0x4B = 0x74        (same)
 *   - SLVT reg 0x49 = 0x00        (same)
 *   - SLVT reg 0x18 — not in ATSC1, but SLVX reg 0x18 = 0
 *   - Additional bank 0xA3 reg 0xA1 (4-byte chip-config write)
 *   - Slave-R (atsc1 sub-block) reg 0x13 = 3 — special SlaveRWriteRegister
 *
 * This is a minimal port — complete fidelity needs SlaveRWrite ports too.
 */
/* External: br_cmd helpers for SLVR proxy via i2c=0x98 + CMD 0xC5 */
extern hdtvmate_error_t br_cmd_slvr_init(it9300_device_t *dev);
extern hdtvmate_error_t br_cmd_slvr_read_setup(it9300_device_t *dev);
extern hdtvmate_error_t br_cmd_slvr_write(it9300_device_t *dev, uint8_t bank,
                                           uint8_t reg, uint8_t val);

static hdtvmate_error_t cxd6801_sltoaa1(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;

    LOG_DBG("SLtoAA1: applying ATSC 1.0 (8VSB) mode transition...");

    /* === Phase 1: SetTSClockModeAndFreq SLVT sequence (mode 5 = ATSC 1.0)
     * Captured byte-exact from sony_atsc1_full_capture.log.
     * All writes to SLVT (0xD8). Multiple bank switches. */
    /* bank 0 + initial register set_bits */
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0xD3, 0x01, 0x01);  /* enable bit 0 */
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0xDE, 0x00, 0x01);  /* clear bit 0 */
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0xDA, 0x00, 0x01);
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0xC4, 0x01, 0x03);  /* lower 2 bits = 01 */
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x00, 0xD1, 0x01, 0x03);
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xD9, 0x08);
    /* TS clock toggle */
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x32, 0x00);
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x33, 0x01);  /* divider for ATSC 1.0 */
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x32, 0x01);
    /* bank 0x01 */
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x01, 0xE7, 0x01, 0x01);
    /* bank 0x10 */
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x10, 0x66, 0x01, 0x01);
    /* bank 0x40 */
    cxd6801_i2c_set_bits(&dev->i2c_demod, 0x40, 0x66, 0x01, 0x01);
    /* bank 0x95 */
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x95, 0x11, 0x60);
    /* bank 0x02 */
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x02, 0xE7, 0x00);

    LOG_DBG("SLtoAA1: SetTSClockModeAndFreq SLVT sequence done");

    /* SLVR (auxiliary 8VSB demod slave) writes captured byte-level via
     * Frida hook on sony_cxd6801_i2c_CommonWriteRegister
     * (sony_atsc1_full_capture.log).
     *
     * KEY FINDING: SLVR is reached via PROXY slave at i2c=0x98 with
     * CMD 0xC5 6-byte packets to reg 0x0A:
     *   [0xC5, bank, reg, val, 0xFF, 0x00]
     *
     * NOT direct i2c=0xC0/0xDA writes (those went to wrong slave
     * which bridge fake-ACKed but chip never received). */
    static const struct { uint8_t bank, reg, val; } slvr_seq[] = {
        {0xA3, 0xA1, 0x77}, {0xA3, 0xA2, 0x77}, {0xA3, 0xA3, 0x77}, {0xA3, 0xA4, 0x27},
        {0xA0, 0x13, 0x03},
        {0x06, 0xA0, 0x31}, {0x06, 0xA1, 0xA5}, {0x06, 0xA2, 0x2E}, {0x06, 0xA3, 0x9F},
        {0x06, 0xA4, 0x2B}, {0x06, 0xA5, 0x99}, {0x06, 0xA6, 0x00}, {0x06, 0xA7, 0xCD},
        {0x06, 0xA8, 0x00}, {0x06, 0xA9, 0xCD}, {0x06, 0xAA, 0x00}, {0x06, 0xAB, 0x00},
        {0x06, 0xAC, 0x2B}, {0x06, 0xAD, 0x9D},
        {0xA0, 0x6F, 0x61}, {0xA0, 0x70, 0xFB}, {0xA0, 0x71, 0x7F},
        {0x09, 0x80, 0x61}, {0x09, 0x81, 0xFB}, {0x09, 0x82, 0x7F},
        {0xA0, 0x73, 0x26}, {0xA0, 0x74, 0x49}, {0xA0, 0x75, 0x7C},
        {0x09, 0x83, 0x26}, {0x09, 0x84, 0x49}, {0x09, 0x85, 0x7C},
        {0x06, 0x71, 0x05},
        {0xA3, 0xA0, 0x10},
    };
    const int n = (int)(sizeof(slvr_seq) / sizeof(slvr_seq[0]));

    /* SLVX (extended slave at i2c=0xDC) — reg 0x17 = 0x0F before SLVR writes.
     * Captured: i2c=0xDC bank 0 reg 0x17 = 0x0F at start of SLtoAA. */
    cxd6801_i2c_t slvx;
    cxd6801_i2c_init(&slvx, dev->bridge, dev->i2c_demod.chip_idx,
                     0xDC, dev->i2c_demod.i2c_bus);
    cxd6801_i2c_write_one(&slvx, 0x00, 0x17, 0x0F);

    /* SLVT (0xD8) bank 0 — system selector + ATSC 1.0 specific bits */
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xA9, 0x00);
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x2C, 0x01);  /* system = ATSC 1.0 */
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x4B, 0x74);
    cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x49, 0x00);

    /* SLVX reg 0x18 = 0x00 (closing) */
    cxd6801_i2c_write_one(&slvx, 0x00, 0x18, 0x00);

    /* Init SLVR proxy (i2c=0x98 reg 0x00=1 + reg 0x48=1) */
    ret = br_cmd_slvr_init(dev->bridge);
    if (ret != HDTVMATE_OK) {
        LOG_WARN("SLVR proxy init failed: %d", ret);
        return ret;
    }

    /* Send 33-write SLVR sequence via 0x98 proxy + CMD 0xC5 */
    int ok = 0;
    for (int i = 0; i < n; i++) {
        ret = br_cmd_slvr_write(dev->bridge,
                                 slvr_seq[i].bank, slvr_seq[i].reg,
                                 slvr_seq[i].val);
        if (ret == HDTVMATE_OK) ok++;
    }
    LOG_INFO("SLtoAA1: SLVR via 0x98 proxy: %d/%d packets sent", ok, n);

    if (ok < n) {
        LOG_WARN("SLtoAA1: only %d/%d SLVR packets accepted", ok, n);
        return HDTVMATE_ERR_TUNE;
    }

    LOG_INFO("SLtoAA1: full ATSC 1.0 mode setup complete (via 0x98 proxy)");
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc1_tune(cxd6801_device_t *dev, uint32_t frequency_khz)
{
    hdtvmate_error_t ret;

    /* CXD6801 DOES support ATSC 1.0 (8VSB). Sony's library has the
     * functions under prefix `sony_cxd6801_demod_atsc_*` (without
     * the '1' — that's why earlier searches missed them).
     *
     * Real ATSC 1.0 path uses BOTH I2C slaves:
     *   SLVT (0xD8) — main demod control (ATSC 3.0 banks)
     *   SLVR (0xDA) — auxiliary slave for 8VSB-specific config
     *
     * The 33-write SlvR sequence in cxd6801_sltoaa1 puts the chip
     * into 8VSB mode (verified ACK on all writes via i2c addr 0xDA). */
    LOG_INFO("ATSC 1.0 (8VSB) tune at %u kHz", frequency_khz);

    /* Power-cycle + re-init like ATSC 3.0 path */
    cxd6801_chip_power_cycle(dev->bridge);
    {
        extern hdtvmate_error_t cxd6801_read_chip_id(cxd6801_device_t *dev);
        cxd6801_read_chip_id(dev);
        dev->initialized = false;
        dev->tuner_initialized = false;
        ret = cxd6801_initialize(dev);
        if (ret != HDTVMATE_OK) {
            LOG_ERR("Re-init after power cycle failed");
            return ret;
        }
    }

    /* SoftReset before mode transition */
    cxd6801_soft_reset(dev);

    /* SLtoAA1 - sleep to active ATSC 1.0 */
    ret = cxd6801_sltoaa1(dev);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("SLtoAA1 mode transition failed");
        return ret;
    }

    /* SLVR is asymmetric on this chip (verified via Sony Frida capture):
     *   writes via PROXY i2c=0x98 + CMD 0xC5 packets (br_cmd_slvr_write)
     *   reads directly at i2c=0x30 with normal bank-select 3-step pattern
     * Lock check (atsc_monitor_SyncStat) reads SLVR via 0x30. */
    dev->slvr_addr = 0x30;

    /* Set state so tuner_tune picks ATSC1 g_param_table entry [9] */
    dev->state = CXD6801_STATE_ACTIVE_ATSC1;

    /* ASCOT3 tune with ATSC 1.0 front-end (gain=0x0C, RF filter=0x03) */
    ret = cxd6801_tuner_tune(dev, frequency_khz, CXD6801_BW_6MHZ);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Tuner tune failed");
        return ret;
    }

    /* Bridge SLVR-read routing setup — Sony emits this between Tune end and
     * SetStreamOutput. Without it, i2c=0x30 reads NACK at the chip. */
    ret = br_cmd_slvr_read_setup(dev->bridge);
    if (ret != HDTVMATE_OK) {
        LOG_WARN("SLVR read setup failed (chip may NACK on i2c=0x30 reads): %d", ret);
        /* Non-fatal — continue, lock check will surface the failure */
    }

    /* SetStreamOutput sequence — captured from sony_atsc1_full_capture.log:
     *   bank 0 select (already there)
     *   read reg 0xA9 (value discarded — sync poke)
     *   bank 0 select (again)
     *   write reg 0xC3 = 0x01  (ENABLE TS output for ATSC 1.0)
     * Sony toggles 0x00/0x01 across retune cycles. Set 0x01 for streaming. */
    {
        uint8_t a9_val;
        cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x00, 0x00);
        cxd6801_i2c_read(&dev->i2c_demod, 0x00, 0xA9, &a9_val, 1);
        cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0x00, 0x00);
        cxd6801_i2c_write_one(&dev->i2c_demod, 0x00, 0xC3, 0x01);  /* ENABLE */
    }

    dev->state = CXD6801_STATE_ACTIVE_ATSC1;
    dev->frequency_khz = frequency_khz;

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_atsc1_check_lock(cxd6801_device_t *dev, bool *locked)
{
    /* Extracted from disassembly of sony_cxd6801_demod_atsc_monitor_SyncStat
     * (@ 0xf5df4) which CheckTSLock uses internally:
     *
     *   read SLVR bank 0xF reg 0x11 → bit 0 = ts_valid (out0)
     *   read SLVR bank 0x9 reg 0x62 → bit 4 = unlock_detector (out1)
     *                                bit 6 (inverted) = early_unlock (out3)
     *   read SLVR bank 0xD reg 0x86 → bit 0 = ts_lock (out2)
     *
     * CheckTSLock:
     *   if (out1 != 0)               → unlock (return 0 lock=0)
     *   else if (out2 != 0 && [+0x2a7] != 0) → check out3 → partial
     *   else                         → locked (lock=1)
     *
     * SLVR i2c is asymmetric: writes via 0x98 proxy + CMD 0xC5; reads
     * direct at i2c=0x30 with bank-select 3-step. Confirmed in Sony's
     * own capture (sony_atsc1_full_capture.log lines 260-294: same
     * three reads at i2c=0x30, returning F.11=0x00 / 9.62=0x51 / D.86=0x00
     * for an unlocked channel). */
    *locked = false;

    if (!dev->slvr_addr) {
        LOG_WARN("ATSC 1.0 lock check: slvr_addr unset (called before tune?)");
        return HDTVMATE_ERR_TUNE;
    }

    cxd6801_i2c_t slvr;
    cxd6801_i2c_init(&slvr, dev->bridge, dev->i2c_demod.chip_idx,
                     dev->slvr_addr, dev->i2c_demod.i2c_bus);

    uint8_t reg_F_11 = 0, reg_9_62 = 0, reg_D_86 = 0;
    hdtvmate_error_t ret;

    ret = cxd6801_i2c_read(&slvr, 0x0F, 0x11, &reg_F_11, 1);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_read(&slvr, 0x09, 0x62, &reg_9_62, 1);
    if (ret != HDTVMATE_OK) return ret;
    ret = cxd6801_i2c_read(&slvr, 0x0D, 0x86, &reg_D_86, 1);
    if (ret != HDTVMATE_OK) return ret;

    uint8_t ts_valid   = reg_F_11 & 0x01;
    uint8_t unlock_det = (reg_9_62 >> 4) & 0x01;
    uint8_t ts_lock    = reg_D_86 & 0x01;

    LOG_DBG("ATSC 1.0 lock: F.11=0x%02x (valid=%d) 9.62=0x%02x (unlock=%d) D.86=0x%02x (lock=%d)",
            reg_F_11, ts_valid, reg_9_62, unlock_det, reg_D_86, ts_lock);

    /* CheckTSLock: locked iff no unlock-detect and ts_lock asserted */
    *locked = (unlock_det == 0) && (ts_lock != 0);
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
