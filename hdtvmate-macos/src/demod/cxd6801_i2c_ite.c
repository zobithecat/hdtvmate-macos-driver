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
 * Select register bank on the i2c context's slave.
 *
 * SLVT (0xD8) and SLVR (0x30 — read-side, see atsc1 lock check) are both
 * bank-based: bank state is per-slave, so we must select bank on the
 * SAME slave we'll then read from. (Earlier this hardcoded SLVT, which
 * meant any non-SLVT context would bank-select on the wrong slave.)
 */
static hdtvmate_error_t cxd6801_i2c_select_bank(cxd6801_i2c_t *i2c, uint8_t bank)
{
    uint8_t tx[8];
    tx[0] = 2;
    tx[1] = i2c->i2c_bus;
    tx[2] = i2c->i2c_addr;  /* 0xD8 (SLVT) or 0x30 (SLVR-read) */
    tx[3] = 0x00;
    tx[4] = bank;
    return br_cmd_send(i2c->bridge, 0x002B, tx, 5, NULL, 0);
}

/*
 * Read registers from CXD6801 demod or tuner.
 *
 * Demod: bank select + reg set + read, all via 0xDC.
 * Tuner: write reg addr to tuner I2C, then read from tuner+1 (read addr).
 */
hdtvmate_error_t cxd6801_i2c_read(cxd6801_i2c_t *i2c, uint8_t bank,
                                   uint8_t reg, uint8_t *data, uint8_t len)
{
    hdtvmate_error_t ret;
    uint8_t tx_reg[64];

    /* Tuner access: write reg addr, then read from tuner addr+1.
     * SLVT (0xD8) and SLVR-read (0x30) are bank-based demod slaves —
     * route through 3-step path. (0xDA/0xC0 kept from earlier wrong
     * SLVR guesses, harmless.)
     *
     * Asymmetric SLVR i2c map (verified via Sony Frida capture):
     *   writes go via PROXY 0x98 + CMD 0xC5 packets (br_cmd_slvr_write)
     *   reads go directly to 0x30 with normal bank-select 3-step pattern */
    if (i2c->i2c_addr != CXD6801_I2C_ADDR_DEMOD &&
        i2c->i2c_addr != CXD6801_I2C_ADDR_WRITE &&
        i2c->i2c_addr != 0xDA &&
        i2c->i2c_addr != 0xC0 &&
        i2c->i2c_addr != 0x30) {
        /* Write register address to tuner */
        tx_reg[0] = 1;
        tx_reg[1] = i2c->i2c_bus;
        tx_reg[2] = i2c->i2c_addr;  /* tuner write addr (0xC2) */
        tx_reg[3] = reg;
        ret = br_cmd_send(i2c->bridge, 0x002B, tx_reg, 4, NULL, 0);
        if (ret != HDTVMATE_OK) return ret;

        /* Read from tuner (addr+1 for read, or same addr on some devices) */
        tx_reg[0] = len;
        tx_reg[1] = i2c->i2c_bus;
        tx_reg[2] = i2c->i2c_addr | 0x01;  /* tuner read addr (0xC3) */
        ret = br_cmd_send(i2c->bridge, 0x002A, tx_reg, 3, data, len);

        LOG_DBG("I2C tuner read: addr=0x%02x reg=0x%02x len=%d data=%02x -> %s",
                i2c->i2c_addr, reg, len,
                len > 0 ? data[0] : 0,
                (ret == HDTVMATE_OK) ? "OK" : "FAIL");
        return ret;
    }

    /*
     * CXD6801 I2C Read:
     * Bank select via 0xC8 (SLV-T write addr), then reg ptr + read via 0xDC.
     *
     * NOTE on addresses:
     *   0xC8 = SLV-T write address (bank select + register write)
     *   0xDC = SLV-T read address (register pointer + data read)
     *   Both are the SAME physical chip, bank state IS shared.
     *
     * CMD 0x2A combined (4-byte) format was tested but returned 0xFF/0x00
     * on this firmware version. Using split: write reg ptr + read.
     */
    /* SLVT/SLVR ops via i2c->i2c_addr — works for both 0xD8 and 0xDA.
     * bank select [0x00, bank] + reg ptr [reg] + read via CMD 0x2A */
    ret = cxd6801_i2c_select_bank(i2c, bank);
    if (ret != HDTVMATE_OK) return ret;

    tx_reg[0] = 1;
    tx_reg[1] = i2c->i2c_bus;
    tx_reg[2] = i2c->i2c_addr;  /* 0xD8 (SLVT) or 0xDA (SLVR) */
    tx_reg[3] = reg;
    ret = br_cmd_send(i2c->bridge, 0x002B, tx_reg, 4, NULL, 0);
    if (ret != HDTVMATE_OK) return ret;

    tx_reg[0] = len;
    tx_reg[1] = i2c->i2c_bus;
    tx_reg[2] = i2c->i2c_addr;
    ret = br_cmd_send(i2c->bridge, 0x002A, tx_reg, 3, data, len);

    LOG_DBG("I2C read: bank=0x%02x reg=0x%02x len=%d data=%02x %02x -> %s",
            bank, reg, len,
            len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
            (ret == HDTVMATE_OK) ? "OK" : "FAIL");
    return ret;
}

/*
 * Write registers to CXD6801 demodulator or tuner.
 *
 * For demod (i2c_addr=0xC8): uses 0xDC for all operations (confirmed working)
 * For tuner (i2c_addr=0xC2): uses 0xC2 directly (via I2C repeater, no bank select)
 *
 * The ASCOT3 tuner has a flat register space (no bank concept),
 * so we skip bank select when accessing the tuner.
 */
hdtvmate_error_t cxd6801_i2c_write(cxd6801_i2c_t *i2c, uint8_t bank,
                                    uint8_t reg, const uint8_t *data, uint8_t len)
{
    hdtvmate_error_t ret;
    uint8_t tx[64];

    if (len + 4 > sizeof(tx)) {
        return HDTVMATE_ERR_INVALID_PARAM;
    }

    /* Tuner access: direct I2C write using tuner address, no bank select */
    if (i2c->i2c_addr != CXD6801_I2C_ADDR_DEMOD &&
        i2c->i2c_addr != CXD6801_I2C_ADDR_WRITE) {
        /* Tuner: write [reg, data...] directly to tuner I2C address */
        tx[0] = len + 1;
        tx[1] = i2c->i2c_bus;
        tx[2] = i2c->i2c_addr;  /* e.g. 0xC2 for ASCOT3 tuner */
        tx[3] = reg;
        memcpy(&tx[4], data, len);
        ret = br_cmd_send(i2c->bridge, 0x002B, tx, len + 4, NULL, 0);

        LOG_TRC("I2C tuner write: addr=0x%02x reg=0x%02x len=%d -> %s",
                i2c->i2c_addr, reg, len, (ret == HDTVMATE_OK) ? "OK" : "FAIL");
        return ret;
    }

    /* Bank select + register write (no 0xF424 wrap — IT9300 firmware
     * may already handle no-stop mode internally for CMD 0x2B; explicit
     * wrap was tried and didn't reduce SLVT NACKs after X_tune burst). */
    ret = cxd6801_i2c_select_bank(i2c, bank);

    if (ret == HDTVMATE_OK) {
        tx[0] = len + 1;
        tx[1] = i2c->i2c_bus;
        tx[2] = CXD6801_I2C_ADDR_SLVT;  /* 0xD8 */
        tx[3] = reg;
        memcpy(&tx[4], data, len);
        ret = br_cmd_send(i2c->bridge, 0x002B, tx, len + 4, NULL, 0);
    }

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
