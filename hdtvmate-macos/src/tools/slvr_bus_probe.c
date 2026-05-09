/*
 * slvr_bus_probe — probe i2c=0x98 on every IT9300 bus (0..3).
 *
 * Hypothesis: chip's SLVR proxy slave is on a different bus than SLVT/SLVX
 * (which we know live on bus 3). Each bus is tried with a 1-byte write to
 * proxy reg 0x00 — if any returns no NACK (CMD 0x2b error 0x15), that bus
 * holds the SLVR slave.
 */
#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;
    hdtvmate_error_t ret;

    g_log_level = LOG_INFO;

    if ((ret = usb_init(&usb)) != HDTVMATE_OK) return 1;
    if (usb_auto_detect(&usb) != HDTVMATE_OK) { fprintf(stderr, "no device\n"); return 1; }
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;
    if (it9300_initialize(&bridge, &usb) != HDTVMATE_OK) return 1;
    if (cxd6801_create(&demod, &bridge, 0) != HDTVMATE_OK) return 1;
    if (cxd6801_initialize(&demod) != HDTVMATE_OK) return 1;

    printf("=== SLVR bus probe ===\n");
    printf("Probe each bus by writing [reg 0x00 = 0x01] to i2c=0x98.\n");
    printf("CMD 0x2b RX status byte: 0x00 = ACK, 0x15 = NACK on bus.\n\n");

    /* Try the proxy_init write (0x98 reg 0x00 = 0x01) on each bus.
     * Format A: [len, bus, addr, sub, data]   — used by chip i2c
     * Format B: [len, addr, sub, data]        — variant without bus
     */
    for (int bus = 0; bus < 4; bus++) {
        printf("[Bus %d / Path A (with bus)] i2c=0x98 reg=0x00 = 0x01: ", bus);
        fflush(stdout);
        uint8_t tx[] = { 1, (uint8_t)bus, 0x98, 0x00, 0x01 };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("ret=%d\n", ret);
    }
    printf("\n");
    for (int bus = 0; bus < 4; bus++) {
        printf("[Bus %d / Path B (no bus, send anyway)] writing dummy: ", bus);
        fflush(stdout);
        /* Just to compare against bus 3 baseline */
        uint8_t tx[] = { 1, 0x98, 0x00, 0x01 };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("ret=%d (path B doesn't really vary by bus)\n", ret);
        break;  /* only need once for baseline */
    }

    /* Also try alternate addrs that COULD be the SLVR proxy on a chip
     * where the addr differs (e.g., 0x9A, 0x9C, etc.) on bus 3 */
    printf("\n=== Alternate addr probe on bus 3 ===\n");
    for (uint8_t addr = 0x90; addr <= 0xA0; addr += 2) {
        printf("[Bus 3] i2c=0x%02x reg=0x00 = 0x01: ", addr);
        fflush(stdout);
        uint8_t tx[] = { 1, 3, addr, 0x00, 0x01 };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("ret=%d\n", ret);
    }

    /* Sony's IT9300_writeGenericRegisters takes ctx[0x8] as the "bus" byte.
     * In DRV_CXD6801_initialize it's loaded from a static struct (relocated
     * at load time, so we can't easily read the value statically). Brute
     * force common non-3 values to see if any unlocks i2c=0x98. */
    printf("\n=== Brute-force [sp+0x18][0] aka bus byte for i2c=0x98 ===\n");
    uint8_t test_buses[] = { 0, 1, 2, 3, 4, 5, 7, 0x10, 0x20, 0x40, 0x80, 0xC0, 0xF0, 0xFF };
    for (size_t i = 0; i < sizeof(test_buses); i++) {
        uint8_t b = test_buses[i];
        printf("[bus=0x%02x] i2c=0x98 reg=0x00 = 0x01: ", b);
        fflush(stdout);
        uint8_t tx[] = { 1, b, 0x98, 0x00, 0x01 };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("ret=%d\n", ret);
    }

    /* Final hypothesis: the kernel DVB-USB driver uses a DIFFERENT USB CMD
     * (not 0x2B) to reach chip-internal proxy slaves. Probe nearby CMDs. */
    printf("\n=== Brute-force USB CMD opcode for i2c=0x98 ===\n");
    uint16_t test_cmds[] = {
        0x002B, /* baseline GENERIC_I2C_WR */
        0x002C, 0x002D, 0x002E, 0x002F,
        0x0030, 0x0031, 0x0032, 0x0033,
        0x0038, 0x0039, 0x003A, 0x003B, 0x003C,
        0x0040, 0x0048, 0x004C,
        0x0050, 0x0058, 0x005C,
        0x0060, 0x0068,
        0x00C4, 0x00C5  /* echo of SLVR write/read CMD bytes */
    };
    for (size_t i = 0; i < sizeof(test_cmds)/sizeof(test_cmds[0]); i++) {
        uint16_t cmd = test_cmds[i];
        printf("[CMD 0x%04x] i2c=0x98 reg=0x00 = 0x01: ", cmd);
        fflush(stdout);
        uint8_t tx[] = { 1, 3, 0x98, 0x00, 0x01 };
        ret = br_cmd_send(&bridge, cmd, tx, sizeof(tx), NULL, 0);
        printf("ret=%d\n", ret);
    }

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
