#include "usb_device.h"
#include <stdio.h>
#include <string.h>

/* Known USB Vendor IDs for HDTV Mate and ITE-based devices */
static const uint16_t ite_vids[] = {
    0x23E2,  /* Zenview (HDTV Mate) */
    0x048D,  /* ITE Tech */
    0x1D19,  /* Dexatek (ITE-based) */
};

/* Default USB transfer timeout (ms) */
#define USB_TIMEOUT_MS 5000

log_level_t g_log_level = LOG_INFO;

hdtvmate_error_t usb_init(usb_device_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    int ret = libusb_init(&dev->ctx);
    if (ret < 0) {
        LOG_ERR("libusb_init failed: %s", libusb_error_name(ret));
        return HDTVMATE_ERR_USB_INIT;
    }
    return HDTVMATE_OK;
}

hdtvmate_error_t usb_open(usb_device_t *dev, uint16_t vid, uint16_t pid)
{
    if (!dev->ctx) {
        return HDTVMATE_ERR_USB_INIT;
    }

    dev->handle = libusb_open_device_with_vid_pid(dev->ctx, vid, pid);
    if (!dev->handle) {
        LOG_ERR("Cannot open USB device %04x:%04x", vid, pid);
        return HDTVMATE_ERR_DEVICE_NOT_FOUND;
    }

    dev->device = libusb_get_device(dev->handle);
    dev->vid = vid;
    dev->pid = pid;

    /* Detach kernel driver if attached (Linux; no-op on macOS usually) */
    if (libusb_kernel_driver_active(dev->handle, 0) == 1) {
        libusb_detach_kernel_driver(dev->handle, 0);
    }

    dev->is_open = true;
    LOG_INFO("Opened USB device %04x:%04x", vid, pid);
    return HDTVMATE_OK;
}

hdtvmate_error_t usb_auto_detect(usb_device_t *dev)
{
    if (!dev->ctx) {
        return HDTVMATE_ERR_USB_INIT;
    }

    libusb_device **list;
    ssize_t cnt = libusb_get_device_list(dev->ctx, &list);
    if (cnt < 0) {
        LOG_ERR("Failed to get device list");
        return HDTVMATE_ERR_USB_INIT;
    }

    hdtvmate_error_t result = HDTVMATE_ERR_DEVICE_NOT_FOUND;

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;

        /* Check against known ITE vendor IDs */
        for (size_t v = 0; v < sizeof(ite_vids) / sizeof(ite_vids[0]); v++) {
            if (desc.idVendor == ite_vids[v]) {
                LOG_INFO("Found ITE device: %04x:%04x (bus %d, addr %d)",
                         desc.idVendor, desc.idProduct,
                         libusb_get_bus_number(list[i]),
                         libusb_get_device_address(list[i]));

                int ret = libusb_open(list[i], &dev->handle);
                if (ret == 0) {
                    dev->device = list[i];
                    dev->vid = desc.idVendor;
                    dev->pid = desc.idProduct;
                    dev->is_open = true;

                    if (libusb_kernel_driver_active(dev->handle, 0) == 1) {
                        libusb_detach_kernel_driver(dev->handle, 0);
                    }

                    result = HDTVMATE_OK;
                    goto done;
                } else {
                    LOG_WARN("Cannot open device: %s", libusb_error_name(ret));
                }
            }
        }
    }

done:
    libusb_free_device_list(list, 1);
    return result;
}

hdtvmate_error_t usb_discover_endpoints(usb_device_t *dev)
{
    if (!dev->handle) {
        return HDTVMATE_ERR_USB_OPEN;
    }

    struct libusb_config_descriptor *config;
    int ret = libusb_get_active_config_descriptor(dev->device, &config);
    if (ret != 0) {
        /* Try config 0 */
        ret = libusb_get_config_descriptor(dev->device, 0, &config);
        if (ret != 0) {
            LOG_ERR("Cannot get config descriptor: %s", libusb_error_name(ret));
            return HDTVMATE_ERR_USB_OPEN;
        }
    }

    uint8_t ep_out = 0, ep_in_cmd = 0, ep_in_ts = 0;

    LOG_INFO("USB config: %d interface(s)", config->bNumInterfaces);

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            LOG_DBG("Interface %d, altsetting %d: %d endpoints",
                    alt->bInterfaceNumber, alt->bAlternateSetting, alt->bNumEndpoints);

            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                uint8_t addr = ep->bEndpointAddress;
                uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;

                LOG_DBG("  EP 0x%02x: type=%d, maxpkt=%d",
                        addr, type, ep->wMaxPacketSize);

                if (type == LIBUSB_TRANSFER_TYPE_BULK) {
                    if (addr & LIBUSB_ENDPOINT_IN) {
                        /* IN endpoints: first is command response, second is TS */
                        if (!ep_in_cmd) {
                            ep_in_cmd = addr;
                        } else if (!ep_in_ts) {
                            ep_in_ts = addr;
                        }
                    } else {
                        /* OUT endpoint: command TX */
                        if (!ep_out) {
                            ep_out = addr;
                        }
                    }
                }
            }

            /* Claim this interface */
            ret = libusb_claim_interface(dev->handle, alt->bInterfaceNumber);
            if (ret == 0) {
                dev->endpoints.iface_num = alt->bInterfaceNumber;
                LOG_INFO("Claimed interface %d", alt->bInterfaceNumber);
            } else {
                LOG_WARN("Cannot claim interface %d: %s",
                         alt->bInterfaceNumber, libusb_error_name(ret));
            }
        }
    }

    libusb_free_config_descriptor(config);

    if (!ep_out || !ep_in_cmd) {
        LOG_ERR("Missing required endpoints (out=0x%02x, in_cmd=0x%02x, in_ts=0x%02x)",
                ep_out, ep_in_cmd, ep_in_ts);
        return HDTVMATE_ERR_USB_OPEN;
    }

    dev->endpoints.ep_tx = ep_out;
    dev->endpoints.ep_rx = ep_in_cmd;
    dev->endpoints.ep_rx_ts = ep_in_ts ? ep_in_ts : ep_in_cmd;

    LOG_INFO("Endpoints discovered: TX=0x%02x, RX=0x%02x, RX_TS=0x%02x",
             dev->endpoints.ep_tx, dev->endpoints.ep_rx, dev->endpoints.ep_rx_ts);

    return HDTVMATE_OK;
}

hdtvmate_error_t usb_bulk_write(usb_device_t *dev, uint8_t endpoint,
                                 uint8_t *data, int length, int *transferred,
                                 unsigned int timeout_ms)
{
    int actual = 0;
    int ret = libusb_bulk_transfer(dev->handle, endpoint, data, length,
                                    &actual, timeout_ms ? timeout_ms : USB_TIMEOUT_MS);
    if (transferred) *transferred = actual;

    if (ret != 0 && ret != LIBUSB_ERROR_TIMEOUT) {
        LOG_ERR("Bulk write failed on EP 0x%02x: %s (sent %d/%d)",
                endpoint, libusb_error_name(ret), actual, length);
        return HDTVMATE_ERR_USB_TRANSFER;
    }
    return HDTVMATE_OK;
}

hdtvmate_error_t usb_bulk_read(usb_device_t *dev, uint8_t endpoint,
                                uint8_t *data, int length, int *transferred,
                                unsigned int timeout_ms)
{
    int actual = 0;
    int ret = libusb_bulk_transfer(dev->handle, endpoint | LIBUSB_ENDPOINT_IN,
                                    data, length, &actual,
                                    timeout_ms ? timeout_ms : USB_TIMEOUT_MS);
    if (transferred) *transferred = actual;

    if (ret != 0 && ret != LIBUSB_ERROR_TIMEOUT) {
        LOG_ERR("Bulk read failed on EP 0x%02x: %s (got %d/%d)",
                endpoint, libusb_error_name(ret), actual, length);
        return HDTVMATE_ERR_USB_TRANSFER;
    }
    return HDTVMATE_OK;
}

void usb_close(usb_device_t *dev)
{
    if (dev->handle) {
        libusb_release_interface(dev->handle, dev->endpoints.iface_num);
        libusb_close(dev->handle);
        dev->handle = NULL;
    }
    if (dev->ctx) {
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
    }
    dev->is_open = false;
    LOG_INFO("USB device closed");
}

void usb_list_all_devices(void)
{
    libusb_context *ctx;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "Failed to init libusb\n");
        return;
    }

    libusb_device **list;
    ssize_t cnt = libusb_get_device_list(ctx, &list);

    printf("\n=== USB Devices ===\n");
    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;

        /* Highlight ITE devices */
        bool is_ite = false;
        for (size_t v = 0; v < sizeof(ite_vids) / sizeof(ite_vids[0]); v++) {
            if (desc.idVendor == ite_vids[v]) is_ite = true;
        }

        printf("  %s Bus %03d Addr %03d: %04x:%04x",
               is_ite ? ">>>" : "   ",
               libusb_get_bus_number(list[i]),
               libusb_get_device_address(list[i]),
               desc.idVendor, desc.idProduct);

        /* Try to get product string */
        libusb_device_handle *h;
        if (libusb_open(list[i], &h) == 0) {
            unsigned char product[256] = {0};
            unsigned char manufacturer[256] = {0};
            if (desc.iManufacturer)
                libusb_get_string_descriptor_ascii(h, desc.iManufacturer, manufacturer, sizeof(manufacturer));
            if (desc.iProduct)
                libusb_get_string_descriptor_ascii(h, desc.iProduct, product, sizeof(product));
            if (manufacturer[0] || product[0])
                printf(" [%s %s]", manufacturer, product);
            libusb_close(h);
        }
        printf("\n");
    }
    printf("===================\n\n");

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}
