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

hdtvmate_error_t it9300_initialize(it9300_device_t *dev, usb_device_t *usb)
{
    memset(dev, 0, sizeof(*dev));
    dev->usb = usb;
    dev->cmd_seq = 0;
    hdtvmate_error_t ret;

    LOG_INFO("IT9300 initialization (confirmed working sequence)...");

    /*
     * CONFIRMED WORKING INIT SEQUENCE (verified via Linux VM + I2C scan):
     * 1. I2C clock configuration
     * 2. GPIO pin setup
     * 3. GPIO reset (0xD8B7 LOW→HIGH, 200ms)
     * 4. Power enable (0xD8E3/E4/E5)
     */

    /* I2C clock speed: bus 1,2,3 = 366kHz */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF6A7, 0x07);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xF103, 0x07);

    /* GPIO configuration */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D4, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D5, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8D3, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B8, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B9, 0x01);

    /* GPIO Reset: D8B7 LOW → 200ms → HIGH (demodulator reset) */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x00);
    br_user_delay(200);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8B7, 0x01);
    br_user_delay(200);

    /* Power enable GPIO */
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E4, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E5, 0x01);
    it9300_write_register(dev, IT9300_PROCESSOR_LINK, 0xD8E3, 0x01);

    /* Wait for CXD6801 to boot */
    br_user_delay(500);

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
    LOG_DBG("Enabling TS port %d", port);
    /* TS port enable register varies. Common approach from af9035.c */
    uint8_t val = 1;
    return it9300_write_register(dev, IT9300_PROCESSOR_LINK,
                                  IT9300_REG_TS_OUTPUT_MODE + port, val);
}

hdtvmate_error_t it9300_disable_ts_port(it9300_device_t *dev, uint8_t port)
{
    uint8_t val = 0;
    return it9300_write_register(dev, IT9300_PROCESSOR_LINK,
                                  IT9300_REG_TS_OUTPUT_MODE + port, val);
}

hdtvmate_error_t it9300_config_output(it9300_device_t *dev)
{
    LOG_DBG("Configuring TS output...");
    /* Default: serial TS output mode */
    hdtvmate_error_t ret;

    ret = it9300_write_register(dev, IT9300_PROCESSOR_LINK,
                                 IT9300_REG_TS_SERIAL, 0x01);
    if (ret != HDTVMATE_OK) return ret;

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
