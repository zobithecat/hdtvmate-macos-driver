#include "it9300.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

/*
 * br_user.c - Platform abstraction layer for IT9300 USB bridge
 *
 * This replaces the Android-specific BrUser functions with macOS equivalents.
 * In the original code (brUser.cpp), these functions provide:
 * - USB bulk transfer (via libusb)
 * - Timing
 * - Critical sections (mutex)
 *
 * The USB protocol is identical on macOS since it uses libusb directly.
 */

/* USB bulk transfer timeout */
#define BR_USB_TIMEOUT_MS  1000

hdtvmate_error_t br_user_bus_tx(usb_device_t *usb, uint8_t *data, int len)
{
    int transferred = 0;
    hdtvmate_error_t ret = usb_bulk_write(usb, usb->endpoints.ep_tx,
                                           data, len, &transferred,
                                           BR_USB_TIMEOUT_MS);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("BrUser_busTx failed: sent %d/%d bytes", transferred, len);
        return ret;
    }

    LOG_TRC("TX %d bytes -> EP 0x%02x", transferred, usb->endpoints.ep_tx);
    return HDTVMATE_OK;
}

hdtvmate_error_t br_user_bus_rx(usb_device_t *usb, uint8_t *data, int len)
{
    int transferred = 0;
    hdtvmate_error_t ret = usb_bulk_read(usb, usb->endpoints.ep_rx,
                                          data, len, &transferred,
                                          BR_USB_TIMEOUT_MS);
    if (ret != HDTVMATE_OK) {
        LOG_ERR("BrUser_busRx failed: got %d/%d bytes", transferred, len);
        return ret;
    }

    LOG_TRC("RX %d bytes <- EP 0x%02x", transferred, usb->endpoints.ep_rx);
    return HDTVMATE_OK;
}

void br_user_delay(uint32_t ms)
{
    usleep(ms * 1000);
}

uint64_t br_user_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
