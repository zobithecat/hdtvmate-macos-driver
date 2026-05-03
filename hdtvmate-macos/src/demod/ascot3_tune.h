/*
 * ascot3_tune.h - ASCOT3 Tuner X_tune register sequence
 *
 * Decompiled from liba3_phy_sony.so X_tune at 0xdf520 via Ghidra.
 *
 * X_tune parameters:
 *   - pTuner: tuner context (contains I2C handle, device address, config tables)
 *   - frequencykHz: target frequency in kHz
 *   - tvSystem: enum (0=ATSC, 0x17/0x18/0x1e=cable variants, etc.)
 *   - vcoCal: VCO calibration flag
 *
 * Register write sequence:
 *   1. reg 0x87, 2 bytes: {0xC4, 0x40} (fixed - oscillator config)
 *   2. reg 0x91, 2 bytes: system-dependent LNA config
 *   3. reg 0x9C, 2 bytes: RF filter mode
 *   4. reg 0x5E, 9 bytes: core tuning data (divider, gain, filter)
 *   5. SetRegisterBits(reg 0x67, mask, value) - PLL control
 *   6. reg 0x68, 17 bytes: RF filter + AGC + gain from tvSystem table
 *
 * Frequency boundaries:
 *   - < 172001 kHz (VHF-Lo): use table offsets 3, 6
 *   - < 464001 kHz (VHF-Hi/UHF-Lo): use table offsets 4, 7
 *   - >= 464001 kHz (UHF): use table offsets 5, 8
 *
 * Key constants:
 *   0x29FE1 = 172001 kHz (VHF boundary)
 *   0x71481 = 464001 kHz (UHF boundary)
 *
 * tvSystem table: 16 bytes per entry, indexed by tvSystem enum
 *   [0]: RF band & 3
 *   [1]: IF frequency setting (-1 = default 0x80)
 *   [2]: IF fine tune & 0xF
 *   [3-5]: gain values for 3 frequency ranges
 *   [6-8]: RF filter values for 3 frequency ranges
 *   [9]: AGC setting 1
 *   [10]: AGC setting 2
 *   [15]: some flag & 1
 *
 * NOTE: Actual frequency (PLL/VCO) is set by X_oscen(), not X_tune().
 * X_tune only configures filters, gain, and AGC for the target frequency.
 *
 * TODO: Decompile X_oscen (0xdf344, 476 bytes) for PLL frequency register writes.
 */

#ifndef ASCOT3_TUNE_H
#define ASCOT3_TUNE_H

#include <stdint.h>

/* Frequency boundaries */
#define ASCOT3_FREQ_VHF_BOUNDARY   172001  /* kHz */
#define ASCOT3_FREQ_UHF_BOUNDARY   464001  /* kHz */

/* TV System enum (from sony_cxd6801_ascot3_tv_system_t) */
#define ASCOT3_TV_SYSTEM_ATSC3     0x00
#define ASCOT3_TV_SYSTEM_ATSC1     0x09  /* approximate */
#define ASCOT3_TV_SYSTEM_CABLE_J83B 0x17

/* ASCOT3 tuner registers */
#define ASCOT3_REG_OSC_CONFIG      0x87
#define ASCOT3_REG_LNA_CONFIG      0x91
#define ASCOT3_REG_RF_FILTER_MODE  0x9C
#define ASCOT3_REG_TUNE_DATA       0x5E  /* 9 bytes */
#define ASCOT3_REG_PLL_CTRL        0x67
#define ASCOT3_REG_RF_AGC_DATA     0x68  /* 17 bytes */

/* Fixed values from X_tune decompile */
#define ASCOT3_OSC_CONFIG_0        0xC4
#define ASCOT3_OSC_CONFIG_1        0x40

/* reg 0x5E data_3 layout (9 bytes starting at data_3[4]): */
/* [0] = 0xEE (fixed)
 * [1] = 0x02 (fixed)
 * [2] = 0x1E (fixed)
 * [3] = vcoCal ? 0x67 : 0x45
 * [4] = xtal-dependent divider (frequency range dependent)
 * [5-8] = gain/filter from tvSystem table + frequency
 */

/* Xtal/crystal type affects data_3[8] divider:
 * xtal=0: UHF=0x08, VHF=0x20
 * xtal=1: UHF=0x0A, VHF=0x29
 * xtal=2: UHF=0x0C, VHF=0x30
 * xtal=3: UHF=0x14, VHF=0x52
 */
static const uint8_t ascot3_divider_uhf[] = {0x08, 0x0A, 0x0C, 0x14};
static const uint8_t ascot3_divider_vhf[] = {0x20, 0x29, 0x30, 0x52};

#endif /* ASCOT3_TUNE_H */
