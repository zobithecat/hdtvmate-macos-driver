#include "usb_device.h"
#include "it9300.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * eeprom_dump - Dump IT9300 EEPROM/register content for analysis
 */

int main(int argc, char *argv[])
{
    usb_device_t usb;
    it9300_device_t bridge;
    hdtvmate_error_t ret;

    g_log_level = LOG_WARN;

    ret = usb_init(&usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "USB init failed\n"); return 1; }
    ret = usb_auto_detect(&usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "Device not found\n"); return 1; }
    ret = usb_discover_endpoints(&usb);
    if (ret != HDTVMATE_OK) { return 1; }

    memset(&bridge, 0, sizeof(bridge));
    bridge.usb = &usb;

    printf("=== IT9300 EEPROM / Register Dump ===\n\n");

    /* Dump EEPROM regions commonly used by ITE SDK */
    static const struct { uint32_t addr; uint8_t len; const char *name; } regions[] = {
        {0x4979, 1, "chip_Type"},
        {0x49AC, 1, "rx_device_count (tuner_count)"},
        {0x49D0, 1, "tuner_ID[0]"},
        {0x49D1, 1, "tuner_ID[1]"},
        {0x49D2, 1, "tuner_ID[2]"},
        {0x49D3, 1, "tuner_ID[3]"},
        {0x49D4, 1, "tuner_ID[4]"},
        {0x49E0, 8, "i2c_addr_set1"},
        {0x49E8, 8, "i2c_addr_set2"},
        {0x4990, 16, "EEPROM block 0x4990"},
        {0x499C, 32, "EEPROM block 0x499C"},
        {0x49AC, 16, "EEPROM block 0x49AC"},
        {0x49BC, 16, "EEPROM block 0x49BC"},
        {0x49CC, 16, "EEPROM block 0x49CC"},
        {0x49DC, 16, "EEPROM block 0x49DC"},
        {0x49EC, 16, "EEPROM block 0x49EC"},
        /* Also check tuner_info area from DRV_IT930x_device_init */
        {0x49FC, 16, "EEPROM block 0x49FC"},
    };

    for (size_t i = 0; i < sizeof(regions)/sizeof(regions[0]); i++) {
        uint8_t data[32] = {0};
        uint8_t len = regions[i].len;
        if (len > 32) len = 32;

        ret = it9300_read_registers(&bridge, IT9300_PROCESSOR_LINK,
                                     regions[i].addr, data, len);

        printf("  0x%04X [%2d]: ", regions[i].addr, len);
        for (int j = 0; j < len; j++) {
            printf("%02x ", data[j]);
        }

        /* Also show as ASCII if printable */
        printf(" | ");
        for (int j = 0; j < len; j++) {
            printf("%c", (data[j] >= 0x20 && data[j] < 0x7F) ? data[j] : '.');
        }

        printf("  (%s)\n", regions[i].name);
    }

    /* Also read the key config registers */
    printf("\n=== Key Config Registers ===\n");
    static const struct { uint32_t addr; const char *name; } regs[] = {
        {0xDA5A, "I2C bus speed"},
        {0xDA1D, "I2C/output ctrl"},
        {0xD820, "TS mode"},
        {0xD8D7, "I2C master port0"},
        {0xD8D8, "I2C master port0 cfg1"},
        {0xD8D9, "I2C master port0 cfg2"},
        {0xF103, "Clock config"},
        {0xF53A, "GPIO/mode config"},
    };

    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        uint8_t val = 0;
        it9300_read_register(&bridge, IT9300_PROCESSOR_LINK, regs[i].addr, &val);
        printf("  0x%04X = 0x%02X  (%s)\n", regs[i].addr, val, regs[i].name);
    }

    /* Try indirect I2C at different addresses */
    printf("\n=== I2C Scan (indirect via 0x4900/0xF000) ===\n");
    static const uint8_t i2c_addrs[] = {0xC0, 0xC2, 0xC8, 0xCA, 0xD0, 0xD2, 0xD8, 0xDA};
    for (size_t i = 0; i < sizeof(i2c_addrs); i++) {
        uint8_t cmd[4] = {0xF5, i2c_addrs[i], 0xFD, 0x01};
        it9300_write_registers(&bridge, IT9300_PROCESSOR_LINK, 0x4900, cmd, 4);
        usleep(5000);
        uint8_t val = 0xFF;
        it9300_read_register(&bridge, IT9300_PROCESSOR_LINK, 0xF000, &val);
        printf("  I2C addr 0x%02X reg 0xFD: 0x%02X %s\n", i2c_addrs[i], val,
               val != 0xFF && val != 0x1A && val != 0x00 ? "<--- DEVICE FOUND?" : "");
    }

    usb_close(&usb);
    return 0;
}
