#include "usb_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * hdtvmate_detect - USB device detection tool
 *
 * Lists all USB devices and highlights ITE-based tuner sticks.
 * When the HDTV Mate is found, displays VID/PID and endpoint info.
 *
 * Usage:
 *   ./hdtvmate_detect              # Auto-detect
 *   ./hdtvmate_detect -v           # Verbose mode
 *   ./hdtvmate_detect --list       # List all USB devices
 *   ./hdtvmate_detect VID PID      # Open specific device
 */

static void print_usage(const char *prog)
{
    printf("GTMedia HDTV Mate USB Device Detector\n\n");
    printf("Usage: %s [options] [VID PID]\n\n", prog);
    printf("Options:\n");
    printf("  -v, --verbose    Enable verbose/debug logging\n");
    printf("  -t, --trace      Enable trace logging\n");
    printf("  --list           List all USB devices\n");
    printf("  VID PID          Open specific device (hex, e.g., 048D 9306)\n");
    printf("\nConnect the HDTV Mate stick and run this tool to discover its USB IDs.\n");
}

int main(int argc, char *argv[])
{
    usb_device_t usb;
    uint16_t vid = 0, pid = 0;
    bool list_only = false;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_log_level = LOG_DEBUG;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--trace") == 0) {
            g_log_level = LOG_TRACE;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (vid == 0) {
            vid = (uint16_t)strtol(argv[i], NULL, 16);
        } else if (pid == 0) {
            pid = (uint16_t)strtol(argv[i], NULL, 16);
        }
    }

    printf("=== GTMedia HDTV Mate USB Detector ===\n\n");

    /* List all devices */
    usb_list_all_devices();

    if (list_only) {
        return 0;
    }

    /* Initialize USB */
    hdtvmate_error_t ret = usb_init(&usb);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "Failed to initialize USB: %d\n", ret);
        return 1;
    }

    /* Open device */
    if (vid && pid) {
        printf("Opening device %04x:%04x...\n", vid, pid);
        ret = usb_open(&usb, vid, pid);
    } else {
        printf("Auto-detecting HDTV Mate...\n");
        ret = usb_auto_detect(&usb);
    }

    if (ret != HDTVMATE_OK) {
        printf("\nDevice not found.\n");
        printf("Make sure the HDTV Mate is connected.\n");
        printf("If the device is visible in the list above but not auto-detected,\n");
        printf("try: %s <VID> <PID>\n", argv[0]);
        usb_close(&usb);
        return 1;
    }

    printf("\n*** HDTV Mate found! ***\n");
    printf("  VID: 0x%04X\n", usb.vid);
    printf("  PID: 0x%04X\n", usb.pid);

    /* Discover endpoints */
    ret = usb_discover_endpoints(&usb);
    if (ret == HDTVMATE_OK) {
        printf("  TX Endpoint:    0x%02X\n", usb.endpoints.ep_tx);
        printf("  RX Endpoint:    0x%02X\n", usb.endpoints.ep_rx);
        printf("  RX_TS Endpoint: 0x%02X\n", usb.endpoints.ep_rx_ts);
        printf("  Interface:      %d\n", usb.endpoints.iface_num);
    }

    printf("\nSave these values for use with other hdtvmate tools.\n");
    printf("Add to include/usb_device.h:\n");
    printf("  #define HDTVMATE_VID  0x%04X\n", usb.vid);
    printf("  #define HDTVMATE_PID  0x%04X\n", usb.pid);

    usb_close(&usb);
    return 0;
}
