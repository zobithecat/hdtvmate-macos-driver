#include "cxd6801.h"
#include <stdio.h>
#include <string.h>

/*
 * cxd6801_i2c_ite.c - I2C communication with CXD6801 through IT9300 bridge
 *
 * CONFIRMED WORKING PARAMETERS (via Linux VM + I2C scan):
 *   - CMD 0x2B (write): [data_len] [bus=3] [addr=0xC8] [data...]
 *   - CMD 0x2A (read):  [read_len] [bus=3] [addr=0xC8]
 *   - Bank select: write [0x00, bank] to addr 0xC8
 *   - Register read: write [reg], then read [data]
 *
 * IT9300 I2C format (from avl6381/it930x.c + Linux af9035 driver):
 *   Write: CMD 0x2B, data = [len, bus, addr<<1, data_bytes...]
 *   Read:  CMD 0x2A, data = [len, bus, addr<<1]
 *   (addr is already in 8-bit format for this device)
 */

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);
extern void br_user_delay(uint32_t ms);

hdtvmate_error_t cxd6801_i2c_init(cxd6801_i2c_t *i2c, it9300_device_t *bridge,
                                   uint8_t chip_idx, uint8_t i2c_addr, uint8_t i2c_bus)
{
    i2c->bridge = bridge;
    i2c->chip_idx = chip_idx;
    i2c->i2c_addr = i2c_addr;
    i2c->i2c_bus = CXD6801_I2C_BUS;  /* Force bus=3 (confirmed) */

    LOG_DBG("I2C init: addr=0x%02x, bus=%d", i2c_addr, i2c->i2c_bus);
    return HDTVMATE_OK;
}

/*
 * I2C Write via CMD 0x2B
 * Format: [data_len] [bus] [addr] [data_bytes...]
 */
static hdtvmate_error_t i2c_raw_write(cxd6801_i2c_t *i2c, const uint8_t *data, uint8_t len)
{
    uint8_t tx[64];
    if (len + 3 > sizeof(tx)) return HDTVMATE_ERR_INVALID_PARAM;

    tx[0] = len;
    tx[1] = i2c->i2c_bus;
    tx[2] = i2c->i2c_addr;
    memcpy(&tx[3], data, len);

    return br_cmd_send(i2c->bridge, 0x002B, tx, len + 3, NULL, 0);
}

/*
 * I2C Read via CMD 0x2A
 * Format: [read_len] [bus] [READ_addr]
 * CXD6801 uses SEPARATE read address (0xDC) from write address (0xC8)!
 */
static hdtvmate_error_t i2c_raw_read(cxd6801_i2c_t *i2c, uint8_t *data, uint8_t len)
{
    uint8_t tx[3];
    tx[0] = len;
    tx[1] = i2c->i2c_bus;
    tx[2] = CXD6801_I2C_ADDR_READ;  /* 0xDC - confirmed! */

    return br_cmd_send(i2c->bridge, 0x002A, tx, 3, data, len);
}

/*
 * Select register bank
 * CXD6801: ALL operations (bank select, reg set, read) go through READ addr (0xDC)
 * Confirmed by testing: using 0xDC for write+read gives correct data!
 */
static hdtvmate_error_t cxd6801_i2c_select_bank(cxd6801_i2c_t *i2c, uint8_t bank)
{
    /* Bank select via READ address (0xDC) - confirmed working */
    uint8_t tx[64];
    uint8_t data[2] = {0x00, bank};
    tx[0] = 2;
    tx[1] = i2c->i2c_bus;
    tx[2] = CXD6801_I2C_ADDR_READ;  /* 0xDC! */
    memcpy(&tx[3], data, 2);
    return br_cmd_send(i2c->bridge, 0x002B, tx, 5, NULL, 0);
}

/*
 * Read registers from CXD6801
 * ALL operations use READ address (0xDC) - confirmed by testing!
 */
hdtvmate_error_t cxd6801_i2c_read(cxd6801_i2c_t *i2c, uint8_t bank,
                                   uint8_t reg, uint8_t *data, uint8_t len)
{
    hdtvmate_error_t ret;

    /* Step 1: Select bank via 0xDC */
    ret = cxd6801_i2c_select_bank(i2c, bank);
    if (ret != HDTVMATE_OK) return ret;

    /* Step 2: Set register address via 0xDC */
    uint8_t tx_reg[64];
    tx_reg[0] = 1;
    tx_reg[1] = i2c->i2c_bus;
    tx_reg[2] = CXD6801_I2C_ADDR_READ;
    tx_reg[3] = reg;
    ret = br_cmd_send(i2c->bridge, 0x002B, tx_reg, 4, NULL, 0);
    if (ret != HDTVMATE_OK) return ret;

    /* Step 3: Read data via 0xDC */
    ret = i2c_raw_read(i2c, data, len);

    LOG_DBG("I2C read: bank=0x%02x reg=0x%02x len=%d data=%02x %02x -> %s",
            bank, reg, len,
            len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
            (ret == HDTVMATE_OK) ? "OK" : "FAIL");
    return ret;
}

/*
 * Write registers to CXD6801
 * ALL operations via 0xDC (confirmed working)
 */
hdtvmate_error_t cxd6801_i2c_write(cxd6801_i2c_t *i2c, uint8_t bank,
                                    uint8_t reg, const uint8_t *data, uint8_t len)
{
    hdtvmate_error_t ret;
    uint8_t tx[64];

    if (len + 4 > sizeof(tx)) {
        return HDTVMATE_ERR_INVALID_PARAM;
    }

    /* Step 1: Select bank via 0xDC */
    ret = cxd6801_i2c_select_bank(i2c, bank);
    if (ret != HDTVMATE_OK) return ret;

    /* Step 2: Write [reg, data...] via 0xDC */
    tx[0] = len + 1;
    tx[1] = i2c->i2c_bus;
    tx[2] = CXD6801_I2C_ADDR_READ;  /* 0xDC */
    tx[3] = reg;
    memcpy(&tx[4], data, len);
    ret = br_cmd_send(i2c->bridge, 0x002B, tx, len + 4, NULL, 0);

    LOG_TRC("I2C write: bank=0x%02x reg=0x%02x len=%d -> %s",
            bank, reg, len, (ret == HDTVMATE_OK) ? "OK" : "FAIL");
    return ret;
}

hdtvmate_error_t cxd6801_i2c_write_one(cxd6801_i2c_t *i2c, uint8_t bank,
                                        uint8_t reg, uint8_t value)
{
    return cxd6801_i2c_write(i2c, bank, reg, &value, 1);
}

hdtvmate_error_t cxd6801_i2c_set_bits(cxd6801_i2c_t *i2c, uint8_t bank,
                                       uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t data;
    hdtvmate_error_t ret;

    ret = cxd6801_i2c_read(i2c, bank, reg, &data, 1);
    if (ret != HDTVMATE_OK) return ret;

    data = (data & ~mask) | (value & mask);
    return cxd6801_i2c_write_one(i2c, bank, reg, data);
}
