#include "it9300.h"
#include "br_firmware.h"
#include <stdio.h>
#include <string.h>

/*
 * it9300.c - IT9300 USB bridge high-level driver
 *
 * Initialization flow (from binary analysis):
 * 1. USB open + endpoint discovery
 * 2. Read EEPROM configuration (tuner/demod types, I2C addresses)
 * 3. Load firmware (from embedded brFirmware_* arrays)
 * 4. Initialize bridge (IT9300_initialize -> DRV_IT930x_Initialize)
 * 5. Configure TS ports
 * 6. Initialize connected demodulator(s)
 */

/* External command functions */
extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);
extern hdtvmate_error_t br_cmd_read_registers(it9300_device_t *dev, uint8_t processor,
                                               uint32_t addr, uint8_t *values, uint8_t len);
extern hdtvmate_error_t br_cmd_write_registers(it9300_device_t *dev, uint8_t processor,
                                                uint32_t addr, const uint8_t *values, uint8_t len);
extern hdtvmate_error_t br_cmd_read_eeprom(it9300_device_t *dev, uint16_t addr,
                                            uint8_t *values, uint8_t len);
extern hdtvmate_error_t br_cmd_load_firmware(it9300_device_t *dev,
                                              const uint8_t *fw_data, uint32_t fw_len);
extern hdtvmate_error_t br_cmd_reboot(it9300_device_t *dev);
extern hdtvmate_error_t br_cmd_query_info(it9300_device_t *dev, uint8_t *info, uint8_t info_len);
extern hdtvmate_error_t br_cmd_i2c_read(it9300_device_t *dev, uint8_t bus,
                                         uint8_t i2c_addr, uint8_t *data, uint8_t len);
extern hdtvmate_error_t br_cmd_i2c_write(it9300_device_t *dev, uint8_t bus,
                                          uint8_t i2c_addr, const uint8_t *data, uint8_t len);
extern void br_user_delay(uint32_t ms);

/* Forward decl — defined later in this file */
static hdtvmate_error_t it9300_set_bit(it9300_device_t *dev, uint16_t reg,
                                        uint8_t pos, uint8_t value);

hdtvmate_error_t it9300_initialize(it9300_device_t *dev, usb_device_t *usb)
{
    memset(dev, 0, sizeof(*dev));
    dev->usb = usb;
    dev->cmd_seq = 0;
    hdtvmate_error_t ret;

    LOG_INFO("IT9300 initialization (Sony byte-exact sequence — live BrCmd capture)...");

    /*
     * Bridge init — REWRITE 2026-05-09 to byte-exact Sony order via live
     * Frida capture of BrCmd_writeRegisters/readRegisters. Previous version
     * had wrong order + wrong DA-range placement (those go in chip-init,
     * not bridge init). Captured sequence in /tmp/sony_bridge3.log.
     */

    /* === Stage 1: power-up + reset with Sony's exact timing ===
     * Sony's D8B7 cycle from live capture (1778331826.331..827.378):
     *   D8B7=01 → 206ms → D8B7=00 → 203ms → D8B7=01 → 134ms → ... → 504ms → D8B7=01
     * Reset transitions need explicit delays (≥200ms) so the chip's
     * analog reset circuit actually triggers. Without delays our pulse
     * is too short and chip never resets — bridge ACKs but chip i2c
     * subsystem stays in stale state. */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA05, 0x01);  /* power on */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x01);  /* GPIO up */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B8, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B9, 0x01);
    br_user_delay(200);  /* Sony: 206ms before reset */
    /* Reset pulse with Sony-matching delays */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x00);
    br_user_delay(200);  /* Sony: 203ms low */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x01);
    /* Additional GPIO power up */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E4, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E5, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E3, 0x01);

    /* === Stage 2: init flags clear === */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4976, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4BFB, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4978, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4977, 0x00);

    /* === Stage 3: I2C master clock (bus 1/2/3 = 366kHz, val 0x07) === */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF6A7, 0x07);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF103, 0x07);

    /* === Stage 4: TS path setup ===
     * Sony reads each register first (RMW pattern). We write final values
     * directly since we know the targets. */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA1A, 0x00);  /* ignore_sync */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF41F, 0x04);  /* dvbt_inten bit 2 */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA10, 0x00);  /* mpeg_full_speed off */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF41A, 0x05);  /* dvbt_en bit 0 + bit 2 */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA1D, 0x01);  /* mp2_sw_rst */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDD11, 0x0F);  /* ep4_tx_en off (clear bit 5) */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDD13, 0x1B);  /* ep4_tx_nak off */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDD11, 0x2F);  /* ep4_tx_en on (set bit 5) */
    /* DD88 = 2-byte burst {0x00, 0x99} (frame size 0x9900) */
    {
        uint8_t fs[2] = { 0x00, 0x99 };
        it9300_write_registers(dev, IT9300_PROCESSOR_LINK, 0xDD88, fs, 2);
    }
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDD0C, 0x80);

    /* === Stage 5: power finalization === */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA05, 0x00);  /* DA05 toggle off */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA06, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA1D, 0x00);  /* release reset */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD920, 0x00);

    /* === Stage 6: slew rate === */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD833, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD830, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD831, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD832, 0x00);

    /* === Stage 7: init flags set === */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4976, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4975, 0x38);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4971, 0x03);

    /* === Stage 8: GPIO config === */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D4, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D5, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D3, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B8, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B9, 0x01);

    /* === Stage 9: final reset cycle (chip warm-up) === */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x00);
    br_user_delay(500);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x01);
    br_user_delay(200);

    /* NOTE: DA34/DA58/DA51/DA73/DA5F/DA60/DA61/DA62/DA4C/DA5A and DA78
     * REMOVED from bridge init. Sony writes these INSIDE
     * DRV_CXD6801_initialize (chip-init phase), not in bridge init.
     * Will be added to cxd6801_initialize. */

    /* Get firmware version */
    uint32_t fw_ver = 0;
    ret = it9300_get_firmware_version(dev, &fw_ver);
    if (ret == HDTVMATE_OK) {
        dev->fw_version = fw_ver;
        LOG_INFO("IT9300 FW: v%u.%u.%u.%u",
                 (fw_ver >> 24) & 0xFF, (fw_ver >> 16) & 0xFF,
                 (fw_ver >> 8) & 0xFF, fw_ver & 0xFF);
    }

    dev->initialized = true;
    LOG_INFO("IT9300 initialized (I2C bus=3, CXD6801 addr=0xC8)");
    return HDTVMATE_OK;

}

hdtvmate_error_t it9300_load_firmware(it9300_device_t *dev)
{
    if (!dev->firmware.codes || dev->firmware.codes_len == 0) {
        LOG_ERR("No firmware data available. Run extract_firmware.py first.");
        return HDTVMATE_ERR_FIRMWARE;
    }

    return br_cmd_load_firmware(dev, dev->firmware.codes, dev->firmware.codes_len);
}

hdtvmate_error_t it9300_reboot(it9300_device_t *dev)
{
    return br_cmd_reboot(dev);
}

hdtvmate_error_t it9300_get_firmware_version(it9300_device_t *dev, uint32_t *version)
{
    uint8_t data[4] = {0};
    hdtvmate_error_t ret = br_cmd_read_registers(dev, IT9300_PROCESSOR_LINK,
                                                  IT9300_REG_FW_VERSION, data, 4);
    if (ret != HDTVMATE_OK) return ret;

    *version = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
               ((uint32_t)data[2] << 8) | data[3];
    return HDTVMATE_OK;
}

hdtvmate_error_t it9300_read_eeprom_config(it9300_device_t *dev)
{
    uint8_t buf[64];
    hdtvmate_error_t ret;

    /* Read base EEPROM region */
    ret = br_cmd_read_registers(dev, IT9300_PROCESSOR_LINK,
                                 IT9300_EEPROM_BASE, buf, 32);
    if (ret != HDTVMATE_OK) return ret;

    /* Parse EEPROM data
     * Layout varies by ITE SDK version, but generally:
     * Offset 0x00: chip type
     * Offset 0x02: TS mode
     * Offset 0x10+: tuner/demod types and I2C addresses
     */
    dev->eeprom.chip_type = buf[0];
    dev->eeprom.ts_mode = buf[2];
    dev->eeprom.rx_device_count = buf[3] ? buf[3] : 1;

    /* Read tuner/demod configuration */
    ret = br_cmd_read_registers(dev, IT9300_PROCESSOR_LINK,
                                 IT9300_EEPROM_TUNERTYPE, buf, 16);
    if (ret != HDTVMATE_OK) return ret;

    for (int i = 0; i < 4; i++) {
        dev->eeprom.tuner_type[i] = buf[i];
        dev->eeprom.demod_type[i] = buf[4 + i];
        dev->eeprom.i2c_addr_tuner[i] = buf[8 + i];
        dev->eeprom.i2c_addr_demod[i] = buf[12 + i];
    }

    return HDTVMATE_OK;
}

hdtvmate_error_t it9300_read_register(it9300_device_t *dev, uint8_t processor,
                                       uint32_t addr, uint8_t *value)
{
    return br_cmd_read_registers(dev, processor, addr, value, 1);
}

hdtvmate_error_t it9300_write_register(it9300_device_t *dev, uint8_t processor,
                                        uint32_t addr, uint8_t value)
{
    return br_cmd_write_registers(dev, processor, addr, &value, 1);
}

hdtvmate_error_t it9300_read_registers(it9300_device_t *dev, uint8_t processor,
                                        uint32_t addr, uint8_t *values, uint8_t len)
{
    return br_cmd_read_registers(dev, processor, addr, values, len);
}

hdtvmate_error_t it9300_write_registers(it9300_device_t *dev, uint8_t processor,
                                         uint32_t addr, const uint8_t *values, uint8_t len)
{
    return br_cmd_write_registers(dev, processor, addr, values, len);
}

hdtvmate_error_t it9300_i2c_read(it9300_device_t *dev, uint8_t i2c_addr,
                                  uint8_t *data, uint8_t len)
{
    return br_cmd_i2c_read(dev, 0, i2c_addr, data, len);
}

hdtvmate_error_t it9300_i2c_write(it9300_device_t *dev, uint8_t i2c_addr,
                                   const uint8_t *data, uint8_t len)
{
    return br_cmd_i2c_write(dev, 0, i2c_addr, data, len);
}

hdtvmate_error_t it9300_read_generic_registers(it9300_device_t *dev,
                                                uint8_t chip_idx,
                                                uint8_t i2c_addr, uint8_t sub_addr,
                                                uint8_t *data, uint8_t len)
{
    /* CMD 0x2A: sub_addr is the I2C sub-address (register within the device) */
    return br_cmd_i2c_read(dev, sub_addr, i2c_addr, data, len);
}

hdtvmate_error_t it9300_write_generic_registers(it9300_device_t *dev,
                                                 uint8_t chip_idx,
                                                 uint8_t i2c_addr, uint8_t sub_addr,
                                                 const uint8_t *data, uint8_t len)
{
    /* CMD 0x2B: sub_addr is the I2C sub-address (register within the device) */
    return br_cmd_i2c_write(dev, sub_addr, i2c_addr, data, len);
}

hdtvmate_error_t it9300_enable_ts_port(it9300_device_t *dev, uint8_t port)
{
    /* Sony's enableTsPort body (verified from /tmp/sony_init_complete.txt)
     * is just ONE write: DA4C = 1. We previously also wrote DA5A=0x1F
     * (from Linux af9035) but Sony does NOT do this — captured trace
     * shows zero DA5A writes anywhere. Removed. */
    LOG_DBG("Enabling TS port %d", port);
    return it9300_write_register(dev, IT9300_PROCESSOR_LINK,
                                  IT9300_REG_TS_OUTPUT_MODE + port, 0x01);
}

hdtvmate_error_t it9300_disable_ts_port(it9300_device_t *dev, uint8_t port)
{
    uint8_t val = 0;
    return it9300_write_register(dev, IT9300_PROCESSOR_LINK,
                                  IT9300_REG_TS_OUTPUT_MODE + port, val);
}

/* Helper: write a specific bit (pos=position, value=0/1) of an IT9300
 * register without disturbing the other bits. Mimics
 * IT9300_writeRegisterBits(handle, port, reg, pos, len=1, value). */
static hdtvmate_error_t it9300_set_bit(it9300_device_t *dev, uint16_t reg,
                                        uint8_t pos, uint8_t value)
{
    uint8_t cur = 0;
    hdtvmate_error_t ret;
    ret = it9300_read_register(dev, IT9300_PROCESSOR_LINK, reg, &cur);
    if (ret != HDTVMATE_OK) return ret;
    if (value) cur |= (1u << pos);
    else       cur &= ~(1u << pos);
    return it9300_write_register(dev, IT9300_PROCESSOR_LINK, reg, cur);
}

hdtvmate_error_t it9300_config_output(it9300_device_t *dev)
{
    /* IT9300_configOutput(port=0) — exact byte-by-byte match of Sony's
     * configOutput function, captured via Frida WRB/WR events at the
     * very moment of IT9300_initialize:
     *
     *   /tmp/sony_init_complete.txt configOutput body:
     *     DA1D bit 0 = 1                 mp2_sw_rst (reset EP4)
     *     DD11 bit 5 = 0                 ep4_tx_en disable
     *     DD13 bit 5 = 0                 ep4_tx_nak disable
     *     DD11 bit 5 = 1                 ep4_tx_en enable
     *     DD88/89 = 0x00, 0x99           frame_size LSB/MSB = 0x9900
     *     DD0C    = 0x80                 packet_size
     *     DA05 bit 0 = 0
     *     DA06 bit 0 = 0
     *     DA1D bit 0 = 0                 release reset
     *     D920    = 0x00
     *
     * Critical streaming-trigger registers we previously omitted:
     *   - DA05/DA06 bit 0 = 0 (TS-output state-machine arming?)
     *   - D920 = 0
     *   - DD88/89 = 0x9900 (Sony-specific frame_size, NOT Linux af9035's
     *     0x0FF9 = 87*188/4)
     */
    LOG_DBG("IT9300 configOutput (Sony exact byte sequence)...");
    hdtvmate_error_t ret;

    ret = it9300_set_bit(dev, 0xDA1D, 0, 1); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDD11, 5, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDD13, 5, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDD11, 5, 1); if (ret) return ret;

    /* DD88 = 0x00, DD89 = 0x99 — Sony writes these as a 2-byte burst
     * starting at DD88. frame_size = 0x9900 = 39168 (NOT 0x0FF9). */
    {
        uint8_t fs[2] = { 0x00, 0x99 };
        ret = it9300_write_registers(dev, IT9300_PROCESSOR_LINK, 0xDD88, fs, 2);
        if (ret) return ret;
    }
    ret = it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDD0C, 0x80);
    if (ret) return ret;

    ret = it9300_set_bit(dev, 0xDA05, 0, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA06, 0, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA1D, 0, 0); if (ret) return ret;

    ret = it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD920, 0x00);
    return ret;
}

hdtvmate_error_t it9300_set_out_ts_type(it9300_device_t *dev)
{
    /* IT9300_setOutTsType(port=0) — exact byte sequence from
     * /tmp/sony_init_complete.txt setOutTsType body (3 bits only):
     *   F41F bit 2 = 1   (dvbt_inten)
     *   DA10 bit 0 = 0   (mpeg_full_speed)
     *   F41A bit 0 = 1   (dvbt_en)
     */
    LOG_DBG("IT9300 setOutTsType...");
    hdtvmate_error_t ret;
    ret = it9300_set_bit(dev, 0xF41F, 2, 1); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA10, 0, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xF41A, 0, 1); if (ret) return ret;
    return HDTVMATE_OK;
}

hdtvmate_error_t it9300_enable_pid_filter(it9300_device_t *dev, uint8_t port,
                                           uint8_t index, uint16_t pid)
{
    uint8_t data[2];
    data[0] = (uint8_t)(pid & 0xFF);
    data[1] = (uint8_t)((pid >> 8) & 0x1F) | 0x20;  /* Enable bit */

    uint32_t addr = IT9300_REG_PID_FILTER_CTRL + (port * 0x40) + (index * 2);
    return it9300_write_registers(dev, IT9300_PROCESSOR_LINK, addr, data, 2);
}

hdtvmate_error_t it9300_disable_pid_filter(it9300_device_t *dev, uint8_t port,
                                            uint8_t index)
{
    uint8_t data[2] = {0, 0};
    uint32_t addr = IT9300_REG_PID_FILTER_CTRL + (port * 0x40) + (index * 2);
    return it9300_write_registers(dev, IT9300_PROCESSOR_LINK, addr, data, 2);
}

hdtvmate_error_t it9300_reset_pid_filter(it9300_device_t *dev, uint8_t port)
{
    /* Disable all PID filter slots */
    for (int i = 0; i < 32; i++) {
        it9300_disable_pid_filter(dev, port, i);
    }
    return HDTVMATE_OK;
}

void it9300_deinit(it9300_device_t *dev)
{
    if (dev->initialized) {
        it9300_disable_ts_port(dev, 0);
        dev->initialized = false;
        LOG_INFO("IT9300 deinitialized");
    }
}
