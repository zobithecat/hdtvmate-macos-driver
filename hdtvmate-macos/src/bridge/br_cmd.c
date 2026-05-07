#include "it9300.h"
#include <stdio.h>
#include <string.h>

/*
 * br_cmd.c - IT9300 bridge command protocol
 *
 * Command frame format (from Linux kernel af9035.c + binary analysis):
 *
 * TX (host -> device):
 *   Byte 0: Length low byte (total frame - 2)
 *   Byte 1: Length high byte
 *   Byte 2: Sequence number (auto-incremented)
 *   Byte 3: Command code
 *   Bytes 4..N-1: Command data
 *   Byte N: Checksum (XOR or sum of bytes 0..N-1)
 *
 * RX (device -> host):
 *   Byte 0: Length low byte
 *   Byte 1: Length high byte
 *   Byte 2: Sequence number (matches TX)
 *   Byte 3: Command code (matches TX)
 *   Byte 4: Status (0 = success)
 *   Bytes 5..M-1: Response data
 *   Byte M: Checksum
 *
 * Checksum is computed as: ~(sum of all bytes) + 1 (two's complement)
 */

/* External functions from br_user.c */
extern hdtvmate_error_t br_user_bus_tx(usb_device_t *usb, uint8_t *data, int len);
extern hdtvmate_error_t br_user_bus_rx(usb_device_t *usb, uint8_t *data, int len);
extern void br_user_delay(uint32_t ms);

/*
 * AF9035/IT930x 16-bit word-based checksum (from Linux kernel af9035.c)
 * Sum of 16-bit words (little-endian), then bitwise complement.
 */
static uint16_t af9035_checksum(const uint8_t *data, int len)
{
    uint32_t checksum = 0;
    for (int i = 0; i < len; i++) {
        if (i % 2)
            checksum += (uint32_t)data[i] << 8;
        else
            checksum += data[i];
    }
    checksum = ~checksum;
    return (uint16_t)(checksum & 0xFFFF);
}

/* Simple byte checksum (fallback) */
static uint8_t compute_checksum(const uint8_t *data, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(~sum + 1);
}

static bool verify_checksum(const uint8_t *data, int len)
{
    (void)data; (void)len;
    return true; /* Skip for now - we'll verify once format is confirmed */
}

/*
 * Send a command and receive response using AF9035/IT930x protocol
 *
 * TX format (from Linux kernel af9035.c):
 *   [DATA_LEN] [SEQ] [CMD] [DATA * DATA_LEN] [CHECKSUM_HI] [CHECKSUM_LO]
 *
 * RX format:
 *   [DATA_LEN] [SEQ] [ERROR] [DATA * DATA_LEN] [CHECKSUM_HI] [CHECKSUM_LO]
 *
 * Total TX frame = DATA_LEN + 5 (1 len + 1 seq + 1 cmd + 2 checksum)
 * Total RX frame = DATA_LEN + 5 (1 len + 1 seq + 1 error + 2 checksum)
 */
hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                              const uint8_t *tx_data, uint8_t tx_len,
                              uint8_t *rx_data, uint8_t rx_len)
{
    uint8_t tx_buf[IT9300_USB_BUF_SIZE];
    uint8_t rx_buf[IT9300_USB_BUF_SIZE];
    hdtvmate_error_t ret;

    /*
     * Endeavour (IT9300) TX frame format (from BrCmd_sendCommand disassembly):
     *
     *   [LEN] [CMD_HI] [CMD_LO] [SEQ] [DATA...] [CHK_HI] [CHK_LO]
     *
     *   LEN = data_len + 5 (counts all bytes after buf[0])
     *   CMD = 16-bit, HI byte first (usually 0x00 for standard commands)
     *   SEQ = auto-incrementing 8-bit
     *   Checksum = 16-bit word sum of bytes 1..end_of_data, then bitwise NOT
     *              (byte 0 / LEN is EXCLUDED from checksum)
     *   Total frame = data_len + 7 bytes
     */
    int initial_len = tx_len + 4;  /* positions 0..(initial_len-1) filled before chk */

    tx_buf[0] = 0;  /* placeholder - set by checksum logic */
    tx_buf[1] = (uint8_t)(cmd >> 8);    /* CMD_HI */
    tx_buf[2] = (uint8_t)(cmd & 0xFF);  /* CMD_LO */
    tx_buf[3] = dev->cmd_seq++;  /* SEQ */
    if (tx_len > 0 && tx_data) {
        memcpy(&tx_buf[4], tx_data, tx_len);
    }

    /* Compute checksum over bytes 1 to (initial_len - 1) */
    uint16_t chk_sum = 0;
    int chk_count = initial_len - 1;  /* number of bytes to checksum */
    for (int i = 0; i < chk_count / 2; i++) {
        chk_sum += ((uint16_t)tx_buf[2*i + 1] << 8) | tx_buf[2*i + 2];
    }
    if (chk_count % 2) {
        chk_sum += (uint16_t)tx_buf[chk_count] << 8;
    }
    chk_sum = ~chk_sum;

    /* Append checksum */
    tx_buf[initial_len] = (uint8_t)(chk_sum >> 8);
    tx_buf[initial_len + 1] = (uint8_t)(chk_sum & 0xFF);

    /* Set LEN = initial_len + 1 */
    tx_buf[0] = (uint8_t)(initial_len + 1);

    int tx_frame_len = initial_len + 2;  /* total frame bytes */

    /* Hex-dump the entire TX frame for debugging */
    {
        char hex[256] = {0};
        int n = (tx_frame_len < 32) ? tx_frame_len : 32;
        for (int i = 0; i < n; i++) {
            snprintf(hex + i*3, 4, "%02x ", tx_buf[i]);
        }
        LOG_TRC("CMD 0x%02x seq=%d: TX[%d]=%s",
                cmd, tx_buf[3], tx_frame_len, hex);
    }

    /* Send command */
    ret = br_user_bus_tx(dev->usb, tx_buf, tx_frame_len);
    if (ret != HDTVMATE_OK) {
        return ret;
    }

    /*
     * Receive response.
     * Endeavour RX format (from BrCmd_sendCommand disassembly):
     *   [LEN] [CMD_HI] [CMD_LO] [DATA...] [CHK_HI] [CHK_LO]
     *   Data at offset 3. Total = rx_len + 5.
     */
    memset(rx_buf, 0, sizeof(rx_buf));
    ret = br_user_bus_rx(dev->usb, rx_buf, IT9300_USB_BUF_SIZE);
    if (ret != HDTVMATE_OK) {
        return ret;
    }

    LOG_DBG("RX [CMD 0x%02x]: %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x",
            cmd,
            rx_buf[0], rx_buf[1], rx_buf[2],
            rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6],
            rx_buf[7], rx_buf[8], rx_buf[9], rx_buf[10],
            rx_buf[11], rx_buf[12]);

    /*
     * Response data offset analysis:
     * Observed: [LEN] [SEQ] [0xFE/0x00] [0xFF-SEQ] [STATUS?] [DATA...]
     *
     * For REG_RD: byte[3] = 0xFF-SEQ, byte[4] = status(01?), byte[5+] = data
     * For I2C:    byte[3] = 0xFF-SEQ, byte[4] = status(ff?), byte[5+] = echo+data
     *
     * Try offset 3 (BrCmd_sendCommand uses offset 3)
     */
    /*
     * From BrCmd_removeChecksum + BrCmd_sendCommand disassembly:
     *   RX: [LEN] [SEQ_ECHO] [ERROR_CODE] [DATA...] [CHK16]
     *   buffer[2] = error code (0 = success)
     *   Data starts at offset 3 (confirmed!)
     */
    uint8_t error_code = rx_buf[2];
    if (error_code != 0) {
        LOG_WARN("CMD 0x%02x error code: 0x%02x", cmd, error_code);
    }

    if (rx_len > 0 && rx_data) {
        memcpy(rx_data, &rx_buf[3], rx_len);
    }

    return HDTVMATE_OK;
}

/*
 * Read registers from IT9300
 *
 * The register address is encoded as:
 * For LINK processor: address is 3 bytes (addr[2:0])
 * For OFDM processor: address is 3 bytes with processor bit set
 */
hdtvmate_error_t br_cmd_read_registers(it9300_device_t *dev, uint8_t processor,
                                        uint32_t addr, uint8_t *values, uint8_t len)
{
    uint8_t tx_data[7];

    /* Pack register read command data:
     * [0] = length
     * [1] = processor
     * [2..4] = address (24-bit, big-endian)
     */
    /*
     * Endeavour register read data format (matches write format):
     * [0] = read_count
     * [1] = processor
     * [2..5] = 4-byte address (big-endian)
     */
    /*
     * From _IT9300_readRegisters: processor determined by addr range:
     *   addr > 0xFF → processor = 2, addr <= 0xFF → processor = 1
     * CMD = (chip_idx << 12) | REG_RD (chip_idx = 0 for single-device)
     * Processor goes in tx_data[1], NOT in CMD.
     */
    uint8_t proc = (addr > 0xFF) ? 2 : 1;
    uint16_t cmd = IT9300_CMD_REG_RD;  /* chip_idx=0 → CMD=0x0000 */

    tx_data[0] = len;
    tx_data[1] = proc;
    tx_data[2] = (uint8_t)((addr >> 24) & 0xFF);
    tx_data[3] = (uint8_t)((addr >> 16) & 0xFF);
    tx_data[4] = (uint8_t)((addr >> 8) & 0xFF);
    tx_data[5] = (uint8_t)(addr & 0xFF);

    return br_cmd_send(dev, cmd, tx_data, 6, values, len);
}

/*
 * Write registers to IT9300
 */
hdtvmate_error_t br_cmd_write_registers(it9300_device_t *dev, uint8_t processor,
                                         uint32_t addr, const uint8_t *values, uint8_t len)
{
    uint8_t tx_data[IT9300_USB_MAX_WRITE];

    if (len + 7 > IT9300_USB_MAX_WRITE) {
        LOG_ERR("Register write too large: %d bytes", len);
        return HDTVMATE_ERR_INVALID_PARAM;
    }

    /*
     * Processor in data field, NOT in CMD. CMD uses chip_idx (=0).
     */
    uint8_t proc = (addr > 0xFF) ? 2 : 1;
    uint16_t cmd = IT9300_CMD_REG_WR;  /* chip_idx=0 → CMD=0x0001 */

    tx_data[0] = len;
    tx_data[1] = proc;
    tx_data[2] = (uint8_t)((addr >> 24) & 0xFF);
    tx_data[3] = (uint8_t)((addr >> 16) & 0xFF);
    tx_data[4] = (uint8_t)((addr >> 8) & 0xFF);
    tx_data[5] = (uint8_t)(addr & 0xFF);
    memcpy(&tx_data[6], values, len);

    return br_cmd_send(dev, cmd, tx_data, 6 + len, NULL, 0);
}

/*
 * Read EEPROM values from IT9300
 */
hdtvmate_error_t br_cmd_read_eeprom(it9300_device_t *dev, uint16_t addr,
                                     uint8_t *values, uint8_t len)
{
    /* EEPROM reads go through the LINK processor memory space */
    return br_cmd_read_registers(dev, IT9300_PROCESSOR_LINK,
                                  IT9300_EEPROM_BASE + addr, values, len);
}

/*
 * Load firmware to IT9300
 *
 * The firmware download protocol:
 * 1. Send FW_DL_BEGIN
 * 2. Send FW_DL with firmware chunks
 * 3. Send FW_DL_END
 * 4. Optionally reboot
 */
hdtvmate_error_t br_cmd_load_firmware(it9300_device_t *dev,
                                       const uint8_t *fw_data, uint32_t fw_len)
{
    hdtvmate_error_t ret;
    uint32_t offset = 0;
    uint8_t chunk[IT9300_USB_MAX_WRITE];

    LOG_INFO("Loading firmware: %u bytes", fw_len);

    /* Begin firmware download */
    ret = br_cmd_send(dev, IT9300_CMD_FW_DL_BEGIN, NULL, 0, NULL, 0);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Firmware download begin failed");
        return ret;
    }

    /* Send firmware in chunks */
    while (offset < fw_len) {
        uint8_t chunk_len = (fw_len - offset > 48) ? 48 : (uint8_t)(fw_len - offset);

        /* Pack chunk: [addr_hi] [addr_lo] [data...] */
        chunk[0] = (uint8_t)((offset >> 8) & 0xFF);
        chunk[1] = (uint8_t)(offset & 0xFF);
        memcpy(&chunk[2], &fw_data[offset], chunk_len);

        ret = br_cmd_send(dev, IT9300_CMD_FW_DL, chunk, chunk_len + 2, NULL, 0);
        if (ret != HDTVMATE_OK) {
            LOG_ERR("Firmware download failed at offset %u", offset);
            return ret;
        }

        offset += chunk_len;
    }

    /* End firmware download */
    ret = br_cmd_send(dev, IT9300_CMD_FW_DL_END, NULL, 0, NULL, 0);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("Firmware download end failed");
        return ret;
    }

    LOG_INFO("Firmware loaded successfully: %u bytes", fw_len);
    return HDTVMATE_OK;
}

/*
 * Reboot the IT9300
 */
hdtvmate_error_t br_cmd_reboot(it9300_device_t *dev)
{
    LOG_INFO("Rebooting IT9300...");
    hdtvmate_error_t ret = br_cmd_send(dev, IT9300_CMD_FW_BOOT, NULL, 0, NULL, 0);
    if (ret != HDTVMATE_OK) {
        /* Reboot may cause USB reset, so timeout is acceptable */
        LOG_WARN("Reboot command returned: %d (may be normal during USB reset)", ret);
    }
    br_user_delay(500);  /* Wait for reboot */
    return HDTVMATE_OK;
}

/*
 * Query device information (firmware version)
 */
hdtvmate_error_t br_cmd_query_info(it9300_device_t *dev, uint8_t *info, uint8_t info_len)
{
    return br_cmd_send(dev, IT9300_CMD_QUERYINFO, NULL, 0, info, info_len);
}

/*
 * Endeavour Generic I2C Write (CMD 0x2B)
 *
 * From IT9300_writeGenericRegisters disassembly:
 *   TX: [data_len] [i2c_addr_8bit] [sub_addr] [data_bytes...]
 *
 * Note: 3rd byte is SUB-ADDRESS (I2C register), NOT bus number!
 * This was confirmed by drvi2c_cxd_ite_Read/Write disassembly.
 */
/* IT9300 reg 0xF424 wrap around I2C ops — Frida trace of Android HDTV
 * Player v2.32 shows 2001 wraps in 30s of normal scan. Without these,
 * EP 0x84 (TS bulk endpoint) stays stalled and bulk_read returns
 * LIBUSB_ERROR_PIPE forever. The wrap is the EP4 state-machine commit.
 *
 * Direction (verified 2026-05-07 from Frida sony_full2.log capture):
 *   begin: 0xF424 = 1   (repeater ON  — demod I2C cmd can pass)
 *   end:   0xF424 = 0   (repeater OFF — close transaction)
 * Earlier version had these inverted (0 on begin, 1 on end), which
 * meant demod cmds happened with repeater OFF → NACK 0x17. */
static inline void br_f424_begin(it9300_device_t *dev) {
    uint8_t v = 1;
    br_cmd_write_registers(dev, IT9300_PROCESSOR_LINK, 0xF424, &v, 1);
}
static inline void br_f424_end(it9300_device_t *dev) {
    uint8_t v = 0;
    br_cmd_write_registers(dev, IT9300_PROCESSOR_LINK, 0xF424, &v, 1);
}

hdtvmate_error_t br_cmd_i2c_write(it9300_device_t *dev, uint8_t sub_addr,
                                   uint8_t i2c_addr, const uint8_t *data, uint8_t len)
{
    uint8_t tx_data[IT9300_USB_MAX_WRITE];
    hdtvmate_error_t ret;

    if (len + 3 > IT9300_USB_MAX_WRITE) {
        return HDTVMATE_ERR_INVALID_PARAM;
    }

    tx_data[0] = len;        /* data length */
    tx_data[1] = i2c_addr;   /* I2C address (8-bit) */
    tx_data[2] = sub_addr;   /* I2C sub-address (register within device) */
    memcpy(&tx_data[3], data, len);

    LOG_DBG("I2C_WR [addr=0x%02x sub=0x%02x len=%d]: %02x %02x %02x %02x",
            i2c_addr, sub_addr, len,
            len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
            len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);

    /* No 0xF424 wrap on plain writes — verified from byte-level
     * Frida capture (sony_payload.log). Sony only wraps the
     * "set reg pointer" sub-command of a 2-stage read; normal
     * writes (with data payload) go direct without wrap. */
    return br_cmd_send(dev, IT9300_CMD_GENERIC_I2C_WR, tx_data, len + 3, NULL, 0);
}

/*
 * Endeavour Generic I2C Read (CMD 0x2A)
 *
 * From IT9300_readGenericRegisters + drvi2c_cxd_ite_Read disassembly:
 *   TX: [read_len] [i2c_addr_8bit] [sub_addr]  (3 bytes)
 *   RX: [data bytes]  (read_len bytes, starting from sub_addr register)
 *
 * This does a combined I2C write-then-read internally:
 *   Write [sub_addr] → Read [data_len bytes]
 */
hdtvmate_error_t br_cmd_i2c_read(it9300_device_t *dev, uint8_t sub_addr,
                                  uint8_t i2c_addr, uint8_t *data, uint8_t len)
{
    hdtvmate_error_t ret;

    LOG_DBG("I2C_RD [addr=0x%02x sub=0x%02x len=%d]", i2c_addr, sub_addr, len);

    /*
     * Sony's 2-stage read pattern (verified byte-by-byte from
     * sony_payload.log monitor_SyncStat polling cycle):
     *
     *   1. F424 = 1                        (repeater ON)
     *   2. I2C_WR  count=1, slave, sub_addr  ← "set reg pointer", NO data
     *   3. F424 = 0                        (repeater OFF)
     *   4. I2C_RD  count=N, slave           ← bare read from current pointer
     *
     * Stage 1-3 wraps the pointer-set; stage 4 is unwrapped.
     * Our previous 1-stage combined read (sub_addr + read in single
     * I2C_RD packet) is NOT what Sony does, and the chip's I2C
     * state machine appears to behave differently in the 2 cases.
     */

    /* Stage 1: F424 = 1 */
    br_f424_begin(dev);

    /* Stage 2: set reg pointer (I2C_WR with no data — len=1 means
     * "1-byte transaction including just the sub_addr") */
    {
        uint8_t set_ptr[3];
        set_ptr[0] = 1;          /* 1-byte transaction */
        set_ptr[1] = i2c_addr;   /* slave addr */
        set_ptr[2] = sub_addr;   /* target register */
        ret = br_cmd_send(dev, IT9300_CMD_GENERIC_I2C_WR, set_ptr, 3, NULL, 0);
        if (ret != HDTVMATE_OK) {
            br_f424_end(dev);
            return ret;
        }
    }

    /* Stage 3: F424 = 0 */
    br_f424_end(dev);

    /* Stage 4: bare read (no sub_addr — chip uses the pointer set above) */
    {
        uint8_t rd_cmd[2];
        rd_cmd[0] = len;
        rd_cmd[1] = i2c_addr;
        ret = br_cmd_send(dev, IT9300_CMD_GENERIC_I2C_RD, rd_cmd, 2, data, len);
    }
    return ret;
}

/*
 * I2C Write-then-Read using CMD 0x2B (write) + CMD 0x2A (read)
 *
 * Two separate Endeavour I2C transactions:
 * 1. CMD 0x2B: Write sub-address/register bytes
 * 2. CMD 0x2A: Read data bytes
 */
hdtvmate_error_t br_cmd_i2c_write_read(it9300_device_t *dev, uint8_t bus,
                                        uint8_t i2c_addr,
                                        const uint8_t *write_data, uint8_t write_len,
                                        uint8_t *read_data, uint8_t read_len)
{
    hdtvmate_error_t ret;

    /* Step 1: I2C Write (set register address) */
    ret = br_cmd_i2c_write(dev, bus, i2c_addr, write_data, write_len);
    if (ret != HDTVMATE_OK) return ret;

    /* Step 2: I2C Read (get data) */
    ret = br_cmd_i2c_read(dev, bus, i2c_addr, read_data, read_len);

    LOG_DBG("I2C WR_RD [bus=%d addr=0x%02x]: wr=%02x rd=%02x %02x %02x %02x",
            bus, i2c_addr,
            write_len > 0 ? write_data[0] : 0,
            read_len > 0 ? read_data[0] : 0, read_len > 1 ? read_data[1] : 0,
            read_len > 2 ? read_data[2] : 0, read_len > 3 ? read_data[3] : 0);

    return ret;
}
