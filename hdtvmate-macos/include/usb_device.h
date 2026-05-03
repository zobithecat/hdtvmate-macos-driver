#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "hdtvmate.h"
#include <libusb.h>

/* Known USB VID/PID for HDTV Mate variants. */
#define ITE_VENDOR_ID       0x048D
#define ZENVIEW_VENDOR_ID   0x23E2  /* Zenview (HDTV Mate manufacturer) */
#define HDTVMATE_VID        0x23E2
#define HDTVMATE_PID        0x2B02

/* USB endpoint types */
typedef struct {
    uint8_t ep_tx;      /* Bulk OUT endpoint for commands */
    uint8_t ep_rx;      /* Bulk IN endpoint for command responses */
    uint8_t ep_rx_ts;   /* Bulk IN endpoint for transport stream data */
    uint8_t iface_num;  /* USB interface number */
} usb_endpoints_t;

/* USB device context */
typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    libusb_device        *device;
    usb_endpoints_t       endpoints;
    uint16_t              vid;
    uint16_t              pid;
    bool                  is_open;
} usb_device_t;

/* Initialize libusb and enumerate devices */
hdtvmate_error_t usb_init(usb_device_t *dev);

/* Find and open HDTV Mate device.
 * If vid/pid are 0, scans all ITE devices. */
hdtvmate_error_t usb_open(usb_device_t *dev, uint16_t vid, uint16_t pid);

/* Auto-detect HDTV Mate by scanning all USB devices */
hdtvmate_error_t usb_auto_detect(usb_device_t *dev);

/* Discover endpoints from device descriptor */
hdtvmate_error_t usb_discover_endpoints(usb_device_t *dev);

/* Bulk transfer wrappers */
hdtvmate_error_t usb_bulk_write(usb_device_t *dev, uint8_t endpoint,
                                 uint8_t *data, int length, int *transferred,
                                 unsigned int timeout_ms);

hdtvmate_error_t usb_bulk_read(usb_device_t *dev, uint8_t endpoint,
                                uint8_t *data, int length, int *transferred,
                                unsigned int timeout_ms);

/* Close device and cleanup */
void usb_close(usb_device_t *dev);

/* List all USB devices (for detection) */
void usb_list_all_devices(void);

#endif /* USB_DEVICE_H */
