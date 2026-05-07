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

    LOG_INFO("IT9300 initialization (Sony Frida-captured sequence)...");

    /*
     * INIT SEQUENCE — captured chronologically from
     * /tmp/sony_init_complete.txt by hooking IT9300_writeRegister
     * during fresh app launch (HDTV Player v2.32). The order matters:
     * power-enable BEFORE GPIO config, init-flags BEFORE TS-bus setup.
     */

    /* GPIO reset (early — D8B7 LOW→HIGH).
     * Note: in our previous version we put GPIO config before reset and
     * power enable AFTER. Sony does the reset first with no GPIO setup. */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x01);

    /* Power enable GPIO */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E4, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E5, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E3, 0x01);

    /* Init flags (purpose unclear — possibly USB framing config) */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4976, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4BFB, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4978, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4977, 0x00);

    /* I2C clock speed (bus 1,2,3 = 366kHz at val 0x07) */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF6A7, 0x07);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF103, 0x07);

    /* DA1A first, then setOutTsType + configOutput inline (Sony's order) */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA1A, 0x00);

    /* setOutTsType bits */
    it9300_set_bit(dev, 0xF41F, 2, 1);
    it9300_set_bit(dev, 0xDA10, 0, 0);
    it9300_set_bit(dev, 0xF41A, 0, 1);

    /* configOutput sequence */
    it9300_set_bit(dev, 0xDA1D, 0, 1);
    it9300_set_bit(dev, 0xDD11, 5, 0);
    it9300_set_bit(dev, 0xDD13, 5, 0);
    it9300_set_bit(dev, 0xDD11, 5, 1);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDD0C, 0x80);
    it9300_set_bit(dev, 0xDA05, 0, 0);
    it9300_set_bit(dev, 0xDA06, 0, 0);
    it9300_set_bit(dev, 0xDA1D, 0, 0);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD920, 0x00);

    /* TS pin direction/polarity config */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD833, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD830, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD831, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD832, 0x00);

    /* Post-init flags */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4976, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4975, 0x38);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0x4971, 0x03);

    /* GPIO pin direction/output config */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D4, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D5, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D3, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B8, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B9, 0x01);

    /* Second GPIO reset */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x00);
    br_user_delay(500);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x01);
    br_user_delay(200);

    /* Wait for CXD6801 to boot */
    br_user_delay(300);

    /* DA5A=0x1F is written right after init returns in Sony's trace.
     * Including it inside init since that's where it lands in our flow. */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA5A, 0x1F);

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
    /* Pre-enable register writes captured chronologically from
     * /tmp/sony_init_complete.txt — these set TS clock divider, sync
     * byte config, packet length, etc. before the actual port enable
     * register. Without them, enableTsPort succeeds but no data flows.
     */
    LOG_DBG("Enabling TS port %d (with full pre-enable sequence)", port);

    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA34, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA58, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA51, 0xBC);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA73, 0x00);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA5F, 0x7A);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA60, 0x61);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA61, 0x33);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA62, 0x00);

    /* The actual port-enable register (Sony writes this last). For
     * port=0 the register is 0xDA4C; offset is + (port specific). */
    hdtvmate_error_t ret = it9300_write_register(dev, IT9300_PROCESSOR_LINK,
                                  IT9300_REG_TS_OUTPUT_MODE + port, 0x01);
    if (ret != HDTVMATE_OK) return ret;

    /* Sony's trace writes 0xDA5A=0x1F a second time right after da4c=1.
     * This pairs with the first da5a=1f written at end of init — likely
     * a "commit" signal that triggers EP4 to start forwarding TS data. */
    return it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xDA5A, 0x1F);
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
    /* Equivalent of IT9300_setOutTsType(handle, port=0) — disassembled
     * from liba3_phy_sony.so @ 0x18ce7c. Writes the streaming-format
     * control bits the IT9300 firmware needs before EP 0x84 will
     * actually forward TS data. Without these, enableTsPort succeeds
     * but bulk reads time out (no data flowing).
     *
     * Verified bit values via Frida-captured WRB events from app's
     * IT9300_setOutTsType call (in /tmp/sony_init_complete.txt):
     *   F41F bit 2 = 1
     *   DA10 bit 0 = 0   (← we previously had 1, breaking streaming)
     *   F41A bit 0 = 1
     *   DA1D bit 0 = 1
     *   DD11 bit 5 = 0, DD13 bit 5 = 0, DD11 bit 5 = 1 (toggle)
     *   DA05 bit 0 = 0   (← we previously had 1)
     *   DA06 bit 0 = 0   (← we previously had 1)
     *   DA1D bit 0 = 0
     */
    LOG_DBG("Configuring TS output (Sony setOutTsType sequence)...");
    hdtvmate_error_t ret;

    ret = it9300_set_bit(dev, 0xF41F, 2, 1); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA10, 0, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xF41A, 0, 1); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA1D, 0, 1); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDD11, 5, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDD13, 5, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDD11, 5, 1); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA05, 0, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA06, 0, 0); if (ret) return ret;
    ret = it9300_set_bit(dev, 0xDA1D, 0, 0); if (ret) return ret;

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
