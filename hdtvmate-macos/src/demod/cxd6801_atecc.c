/*
 * cxd6801_atecc.c — ATECC secure-element unlock sequence for CXD6801.
 *
 * The CXD6801 chip locks its SLVR (ATSC 1.0 demod proxy at i2c=0x98)
 * until a 4-command ATECC handshake completes at i2c=0xC8. Sony's app
 * runs this during chip-init via Microchip CryptoAuthLib.
 *
 * Captured exact byte sequence via Frida (BrCmd_sendCommand hook):
 *
 *   1. Random:   TX [03 07 1B 00 00 00 24 cd]
 *                RX 34B chip random
 *   2. Nonce:    TX [03 1B 16 00 00 00 + first 20B of step1 RX + CRC]
 *                RX 34B (chip TempKey result)
 *   3. MAC:      TX [03 07 08 41 00 00 2d e7]
 *                RX 34B (computed MAC, host doesn't use)
 *   4. Read:     TX [03 07 02 80 00 00 09 ad]
 *                RX 34B (config zone — chip ID + serial)
 *
 * Each command followed by:
 *   READ 1B status (typically 0x23)
 *   READ 34B response
 *   WRITE [02] commit/idle byte
 *
 * No host-side cryptography needed — chip computes MAC internally using
 * its own slot-0 key. Host just relays bytes and constructs CRC.
 */
#include "cxd6801.h"
#include <string.h>
#include <stdio.h>

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);
extern void br_user_delay(uint32_t ms);

/*
 * ATECC CRC-16 — polynomial 0x8005, init 0x0000, no reflect, no XOR out.
 * Standard CryptoAuthLib atCalcCrc.
 */
static void atecc_crc(const uint8_t *data, uint8_t len, uint8_t crc_out[2])
{
    uint16_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < 8; b++) {
            uint16_t bit = (data[i] >> b) & 1;
            uint16_t feedback = ((crc >> 15) & 1) ^ bit;
            crc <<= 1;
            if (feedback) crc ^= 0x8005;
        }
    }
    crc_out[0] = crc & 0xFF;          /* low byte first (verified vs Sony captures) */
    crc_out[1] = (crc >> 8) & 0xFF;
}

/* Send an ATECC command packet to i2c=0xC8.
 * cmd_data layout: [count, opcode, p1, p2_lo, p2_hi, ...data] (no CRC — we add it).
 * cmd_len = len of cmd_data (count byte + body, NO crc). */
static hdtvmate_error_t atecc_send(it9300_device_t *bridge,
                                    const uint8_t *cmd_data, uint8_t cmd_len)
{
    /* USB packet payload: [len, bus, addr=0xC8, word_addr=0x03, ...cmd_data, crc_lo, crc_hi] */
    uint8_t tx[64];
    if (cmd_len + 6 > sizeof(tx)) return HDTVMATE_ERR_INVALID_PARAM;

    tx[0] = cmd_len + 3;     /* i2c data len: word_addr(1) + cmd_data + CRC(2) */
    tx[1] = 0x03;            /* bus 3 */
    tx[2] = 0xC8;            /* ATECC i2c addr */
    tx[3] = 0x03;            /* word_address: cmd_buffer write */
    memcpy(&tx[4], cmd_data, cmd_len);
    atecc_crc(cmd_data, cmd_len, &tx[4 + cmd_len]);

    return br_cmd_send(bridge, IT9300_CMD_GENERIC_I2C_WR, tx, 4 + cmd_len + 2, NULL, 0);
}

/* Read 1-byte status from ATECC. Status 0x23 (or similar) means ready.
 * 0xFF means chip still processing — caller may poll. */
static hdtvmate_error_t atecc_read_status(it9300_device_t *bridge, uint8_t *status)
{
    uint8_t tx[3] = { 1, 0x03, 0xC8 };
    return br_cmd_send(bridge, IT9300_CMD_GENERIC_I2C_RD, tx, 3, status, 1);
}

/* Read 34-byte response (1 length byte + 32 data + 2 CRC, but length is in first byte). */
static hdtvmate_error_t atecc_read_response(it9300_device_t *bridge,
                                             uint8_t *resp, uint8_t len)
{
    uint8_t tx[3] = { len, 0x03, 0xC8 };
    return br_cmd_send(bridge, IT9300_CMD_GENERIC_I2C_RD, tx, 3, resp, len);
}

/* Send idle byte 0x02 to ATECC (commit / put chip in idle). */
static hdtvmate_error_t atecc_idle(it9300_device_t *bridge)
{
    uint8_t tx[4] = { 1, 0x03, 0xC8, 0x02 };
    return br_cmd_send(bridge, IT9300_CMD_GENERIC_I2C_WR, tx, 4, NULL, 0);
}

/* Run one full ATECC command:
 *   send → wait → read status (poll) → read 34B response → idle */
static hdtvmate_error_t atecc_exec(it9300_device_t *bridge,
                                    const uint8_t *cmd_data, uint8_t cmd_len,
                                    uint8_t *resp, uint8_t resp_len,
                                    uint32_t exec_delay_ms)
{
    hdtvmate_error_t ret = atecc_send(bridge, cmd_data, cmd_len);
    if (ret != HDTVMATE_OK) {
        LOG_WARN("ATECC send failed (op=0x%02x): %d", cmd_data[1], ret);
        return ret;
    }

    br_user_delay(exec_delay_ms);  /* ATECC needs time to compute */

    /* Poll status — Sony's captures show chip returns 0x23 when ready */
    uint8_t status = 0xFF;
    int polls;
    for (polls = 0; polls < 30; polls++) {
        ret = atecc_read_status(bridge, &status);
        if (ret == HDTVMATE_OK && status != 0xFF && status != 0x00) break;
        br_user_delay(1);
    }
    LOG_DBG("ATECC op=0x%02x status=0x%02x after %d polls", cmd_data[1], status, polls);

    /* Read response (34 bytes typical: 1 length + 32 data + 2 CRC) */
    if (resp && resp_len > 0) {
        ret = atecc_read_response(bridge, resp, resp_len);
        if (ret != HDTVMATE_OK) {
            LOG_WARN("ATECC read resp failed (op=0x%02x): %d", cmd_data[1], ret);
            return ret;
        }
        LOG_DBG("ATECC op=0x%02x resp[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x",
                cmd_data[1], resp[0], resp[1], resp[2], resp[3],
                resp[4], resp[5], resp[6], resp[7]);
    }

    /* Commit idle */
    return atecc_idle(bridge);
}

/*
 * ATECC unlock for SLVR — runs the 4-command auth handshake Sony's app does.
 * Call once during chip-init, AFTER bridge init and BEFORE SLVT/SLVX writes.
 */
hdtvmate_error_t cxd6801_atecc_unlock_slvr(cxd6801_device_t *dev)
{
    hdtvmate_error_t ret;
    uint8_t resp[40];

    LOG_INFO("ATECC: starting SLVR unlock handshake at i2c=0xC8");

    /* All ATECC commands need ~10-25ms exec time on chip side. Sony's
     * captures show consistent ~10ms RTT. Set minimum 15ms for all to
     * avoid early polling that returns chip-busy state. */
    const uint32_t EXEC_DELAY = 15;

    /* === Step 1: Random (opcode 0x1B) ===
     * Generates 32 bytes of random — chip's RandOut. */
    {
        uint8_t cmd[5] = { 0x07, 0x1B, 0x00, 0x00, 0x00 };
        ret = atecc_exec(dev->bridge, cmd, sizeof(cmd), resp, 34, EXEC_DELAY);
        if (ret != HDTVMATE_OK) { LOG_WARN("ATECC Random failed: %d", ret); return ret; }
        LOG_DBG("ATECC Random: %02x %02x %02x %02x ...", resp[0], resp[1], resp[2], resp[3]);
    }

    /* === Step 2: Nonce(mode=0, NumIn=first 20B of step 1) === */
    {
        uint8_t cmd[5 + 20] = { 0x1B, 0x16, 0x00, 0x00, 0x00 };
        memcpy(&cmd[5], &resp[0], 20);
        ret = atecc_exec(dev->bridge, cmd, sizeof(cmd), resp, 34, EXEC_DELAY);
        if (ret != HDTVMATE_OK) { LOG_WARN("ATECC Nonce failed: %d", ret); return ret; }
    }

    /* === Step 3: MAC(mode=0x41, key=0) === */
    {
        uint8_t cmd[5] = { 0x07, 0x08, 0x41, 0x00, 0x00 };
        ret = atecc_exec(dev->bridge, cmd, sizeof(cmd), resp, 34, EXEC_DELAY);
        if (ret != HDTVMATE_OK) { LOG_WARN("ATECC MAC failed: %d", ret); return ret; }
    }

    /* === Step 4: Read(zone=config, 32B from offset 0) === */
    {
        uint8_t cmd[5] = { 0x07, 0x02, 0x80, 0x00, 0x00 };
        ret = atecc_exec(dev->bridge, cmd, sizeof(cmd), resp, 34, EXEC_DELAY);
        if (ret != HDTVMATE_OK) { LOG_WARN("ATECC Read failed: %d", ret); return ret; }
        LOG_DBG("ATECC config: %02x %02x %02x %02x %02x %02x %02x %02x",
                resp[0], resp[1], resp[2], resp[3],
                resp[4], resp[5], resp[6], resp[7]);
    }

    LOG_INFO("ATECC: SLVR unlock handshake complete");
    return HDTVMATE_OK;
}
