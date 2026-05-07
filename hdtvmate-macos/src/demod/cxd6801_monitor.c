#include "cxd6801.h"
#include <stdio.h>
#include <string.h>

/*
 * cxd6801_monitor.c - Signal quality monitoring
 *
 * Implements functions from liba3_phy_sony.so:
 * - sony_cxd6801_demod_atsc3_monitor_SyncStat
 * - sony_cxd6801_demod_atsc3_monitor_SNR
 * - sony_cxd6801_demod_atsc3_monitor_AveragedSNR
 * - sony_cxd6801_demod_atsc3_monitor_PreLDPCBER
 * - sony_cxd6801_demod_atsc3_monitor_PreBCHBER
 * - sony_cxd6801_demod_atsc3_monitor_CarrierOffset
 * - sony_cxd6801_demod_atsc3_monitor_L1Basic
 * - sony_cxd6801_demod_atsc3_monitor_Bootstrap
 * - sony_cxd6801_demod_atsc3_monitor_PLPList
 * - sony_cxd6801_demod_atsc3_monitor_IFAGCOut
 *
 * TODO: All register addresses need to be extracted via Ghidra.
 * Current registers are educated guesses from CXD2880 patterns.
 */

hdtvmate_error_t cxd6801_monitor_sync_stat(cxd6801_device_t *dev, uint8_t *sync_stat)
{
    /* Read synchronization status
     * From sony_cxd6801_demod_atsc3_monitor_SyncStat()
     * Values: 0=not started, 1=searching, 5=locked, 6=ALP locked
     */
    return cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x10, sync_stat, 1);
}

hdtvmate_error_t cxd6801_monitor_snr(cxd6801_device_t *dev, int32_t *snr_x100)
{
    uint8_t data[3] = {0};
    uint8_t flag = 0;
    hdtvmate_error_t ret;

    /*
     * Read instantaneous SNR.
     *
     * From disassembly of sony_cxd6801_demod_atsc3_monitor_SNR @ 0xf2954:
     *   SetBank(0x90)
     *   ReadRegister(reg=0x28, len=3)   → SNR raw 24-bit (byte order: see below)
     *   ReadRegister(reg=0x58, len=1)   → branch selector flag
     *
     * Raw assembly:
     *   ldurb w9, [x29, #-0xc]          ; byte 0  (reg 0x28+0)
     *   ldurb w8, [x29, #-0xb]          ; byte 1  (reg 0x28+1)
     *   lsl   w8, w8, #8                ; b1 << 8
     *   bfi   w8, w9, #16, #8           ; w8 |= (b0 << 16)
     *   ldurb w9, [x29, #-0xa]          ; byte 2  (reg 0x28+2)
     *   orr   w8, w8, w9                ; w8 |= b2
     * → raw_24 = (byte0 << 16) | (byte1 << 8) | byte2
     *
     * The flag at reg 0x58 selects which constants are used in the
     * subsequent SNR math (linear→dB conversion). For now we report the
     * raw 24-bit value scaled by 100/8 — same scaling as before but on
     * the correct register. Full conversion to dB×100 requires the
     * lookup-table math from f2b94..f2c60.
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x90, 0x28, data, 3);
    if (ret != HDTVMATE_OK) return ret;
    cxd6801_i2c_read(&dev->i2c_demod, 0x90, 0x58, &flag, 1);

    uint32_t raw_24 = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    *snr_x100 = (int32_t)(raw_24 * 100 / 8);

    LOG_DBG("SNR: raw_24=0x%06x flag=0x%02x -> %d.%02d dB",
            raw_24, flag, *snr_x100 / 100, *snr_x100 % 100);
    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_avg_snr(cxd6801_device_t *dev, int32_t *snr_x100)
{
    uint8_t data[2] = {0};
    hdtvmate_error_t ret;

    /* Averaged SNR from sony_cxd6801_demod_atsc3_monitor_AveragedSNR() */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x2A, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *snr_x100 = (int32_t)(raw * 100 / 8);

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_pre_ldpc_ber(cxd6801_device_t *dev,
                                               uint32_t *num, uint32_t *den)
{
    uint8_t data[4] = {0};
    hdtvmate_error_t ret;

    /* Pre-LDPC BER from sony_cxd6801_demod_atsc3_monitor_PreLDPCBER() */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x50, data, 4);
    if (ret != HDTVMATE_OK) return ret;

    *num = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    *den = (uint32_t)data[3] ? (uint32_t)data[3] * 64800 : 1;

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_pre_bch_ber(cxd6801_device_t *dev,
                                              uint32_t *num, uint32_t *den)
{
    uint8_t data[4] = {0};
    hdtvmate_error_t ret;

    /* Pre-BCH BER from sony_cxd6801_demod_atsc3_monitor_PreBCHBER() */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x54, data, 4);
    if (ret != HDTVMATE_OK) return ret;

    *num = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    *den = (uint32_t)data[3] ? (uint32_t)data[3] * 64800 : 1;

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_carrier_offset(cxd6801_device_t *dev, int32_t *offset_hz)
{
    uint8_t data[3] = {0};
    hdtvmate_error_t ret;

    /* Carrier frequency offset from sony_cxd6801_demod_atsc3_monitor_CarrierOffset() */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x40, data, 3);
    if (ret != HDTVMATE_OK) return ret;

    int32_t raw = ((int32_t)data[0] << 16) | ((int32_t)data[1] << 8) | data[2];
    /* Sign extend 24-bit to 32-bit */
    if (raw & 0x800000) raw |= 0xFF000000;

    /* TODO: Confirm conversion formula from Ghidra */
    *offset_hz = raw;

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_l1_basic(cxd6801_device_t *dev, cxd6801_l1_basic_t *l1)
{
    uint8_t data[16] = {0};
    hdtvmate_error_t ret;

    /*
     * Read L1 Basic signaling.
     * From sony_cxd6801_demod_atsc3_monitor_L1Basic():
     * Contains broadcast parameters like FFT size, guard interval, num PLPs, etc.
     *
     * TODO: Extract register layout from Ghidra
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x92, 0x10, data, 16);
    if (ret != HDTVMATE_OK) return ret;

    /* Parse L1 basic fields - layout TBD from Ghidra */
    l1->version = data[0] & 0x07;
    l1->mimo = (data[0] >> 3) & 0x01;
    l1->lls_flag = (data[0] >> 4) & 0x01;
    l1->time_info = (data[1] >> 0) & 0x0F;
    l1->papr_reduction = (data[1] >> 4) & 0x0F;
    l1->frame_length_mode = data[2] & 0x01;
    l1->frame_length = ((uint16_t)data[3] << 8) | data[4];
    l1->num_subframes = data[5];
    l1->num_plps = data[6];
    l1->fft_size = data[7];
    l1->guard_interval = data[8];

    LOG_INFO("L1 Basic: version=%d, PLPs=%d, FFT=%d, GI=%d",
             l1->version, l1->num_plps, l1->fft_size, l1->guard_interval);

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_bootstrap(cxd6801_device_t *dev, cxd6801_bootstrap_t *bs)
{
    uint8_t data[4] = {0};
    hdtvmate_error_t ret;

    /* Bootstrap info from sony_cxd6801_demod_atsc3_monitor_Bootstrap() */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x92, 0x30, data, 4);
    if (ret != HDTVMATE_OK) return ret;

    bs->version = data[0];
    bs->ea_wake_up = data[1] & 0x01;
    bs->min_time_to_next = data[2];
    bs->system_bandwidth = data[3] & 0x03;
    bs->bsr_coefficient = (data[3] >> 2) & 0x07;

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_plp_list(cxd6801_device_t *dev,
                                           uint8_t *plp_ids, uint8_t *count)
{
    uint8_t data[65] = {0};
    hdtvmate_error_t ret;

    /*
     * Get list of available PLPs on current channel.
     * From sony_cxd6801_demod_atsc3_monitor_PLPList():
     * First byte is count, followed by PLP IDs.
     */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x92, 0x40, data, 65);
    if (ret != HDTVMATE_OK) return ret;

    *count = data[0];
    if (*count > 64) *count = 64;

    memcpy(plp_ids, &data[1], *count);

    LOG_INFO("PLP list: %d PLPs found", *count);
    for (int i = 0; i < *count; i++) {
        LOG_DBG("  PLP[%d] = %d", i, plp_ids[i]);
    }

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_rf_level(cxd6801_device_t *dev, int32_t *rf_dbm)
{
    uint8_t data[2] = {0};
    hdtvmate_error_t ret;

    /* RF power level from sony_cxd6801_integ_atsc3_monitor_RFLevel() */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x60, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    /* TODO: Confirm conversion from Ghidra */
    *rf_dbm = (int32_t)raw - 32768;  /* Placeholder conversion */

    return HDTVMATE_OK;
}

hdtvmate_error_t cxd6801_monitor_ifagc(cxd6801_device_t *dev, uint32_t *ifagc)
{
    uint8_t data[2] = {0};
    hdtvmate_error_t ret;

    /* IFAGC output from sony_cxd6801_demod_atsc3_monitor_IFAGCOut() */
    ret = cxd6801_i2c_read(&dev->i2c_demod, 0x91, 0x20, data, 2);
    if (ret != HDTVMATE_OK) return ret;

    *ifagc = ((uint32_t)data[0] << 8) | data[1];
    return HDTVMATE_OK;
}
