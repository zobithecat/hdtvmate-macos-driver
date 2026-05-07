#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * capture_thread.c - USB bulk capture and processing threads
 *
 * Based on SonyPHYAndroid.cpp threads:
 * - captureThread (line ~1811): reads USB bulk data from ENDPOINT_RX_TS
 * - processThread (line ~1830): parses TLV/ALP packets
 * - statusThread  (line ~1849): monitors signal quality
 *
 * Data flow:
 *   USB bulk IN -> circular_buffer -> processThread -> callback
 */

/* USB read buffer size (128 KB per read) */
#define USB_READ_BUF_SIZE  (128 * 1024)

/* Circular buffer size (8 MB) */
#define RING_BUFFER_SIZE   (8 * 1024 * 1024)

extern void br_user_delay(uint32_t ms);

/*
 * Capture thread - continuously reads USB bulk data
 */
static void *capture_thread_func(void *arg)
{
    capture_context_t *ctx = (capture_context_t *)arg;
    uint8_t *read_buf = (uint8_t *)malloc(USB_READ_BUF_SIZE);

    if (!read_buf) {
        LOG_ERR("Failed to allocate USB read buffer");
        return NULL;
    }

    LOG_INFO("Capture thread started (EP 0x%02x)", ctx->usb->endpoints.ep_rx_ts);

    /* Clear any stale halt on the TS endpoint before the first bulk read.
     * The IT9300 sometimes leaves EP 0x84 in a stalled state after
     * configuration; without clear_halt the first read returns
     * LIBUSB_ERROR_PIPE forever. */
    {
        extern int libusb_clear_halt(struct libusb_device_handle *, unsigned char);
        libusb_clear_halt((struct libusb_device_handle *)ctx->usb->handle,
                          ctx->usb->endpoints.ep_rx_ts | 0x80);
    }

    while (ctx->should_run) {
        int transferred = 0;
        hdtvmate_error_t ret = usb_bulk_read(ctx->usb,
                                              ctx->usb->endpoints.ep_rx_ts,
                                              read_buf, USB_READ_BUF_SIZE,
                                              &transferred, 1000);

        if (ret == HDTVMATE_OK && transferred > 0) {
            circular_buffer_write(&ctx->ring_buffer, read_buf, transferred);
            ctx->bytes_received += transferred;
            LOG_TRC("USB read: %d bytes (total: %llu)",
                    transferred, (unsigned long long)ctx->bytes_received);
        } else if (ret != HDTVMATE_OK) {
            ctx->errors++;
            if (ctx->errors > 100) {
                LOG_ERR("Too many USB errors, stopping capture");
                break;
            }
        }
    }

    free(read_buf);
    LOG_INFO("Capture thread stopped (received %llu bytes, %u errors)",
             (unsigned long long)ctx->bytes_received, ctx->errors);
    return NULL;
}

/*
 * Process thread - reads from circular buffer and dispatches to callback
 *
 * The original SonyPHYAndroid processes TLV (Type-Length-Value) framing
 * from the IT9300, which contains ALP (ATSC Link-layer Protocol) packets.
 *
 * TLV frame format (from IT9300):
 *   [0x47] [PID_HI] [PID_LO] [FLAGS] [PAYLOAD...]
 *
 * For ATSC 3.0, the payload contains ALP packets.
 * For ATSC 1.0, the payload is standard MPEG-2 TS.
 *
 * For now, we pass raw data directly to the callback.
 * Full TLV/ALP parsing will be added once we capture real device data.
 */
static void *process_thread_func(void *arg)
{
    capture_context_t *ctx = (capture_context_t *)arg;
    uint8_t *proc_buf = (uint8_t *)malloc(USB_READ_BUF_SIZE);

    if (!proc_buf) {
        LOG_ERR("Failed to allocate process buffer");
        return NULL;
    }

    LOG_INFO("Process thread started");

    while (ctx->should_run) {
        size_t avail = circular_buffer_available(&ctx->ring_buffer);
        if (avail == 0) {
            br_user_delay(10);
            continue;
        }

        size_t to_read = (avail > USB_READ_BUF_SIZE) ? USB_READ_BUF_SIZE : avail;
        size_t read_count = circular_buffer_read(&ctx->ring_buffer, proc_buf, to_read);

        if (read_count > 0 && ctx->callback) {
            ctx->callback(proc_buf, read_count, ctx->callback_data);
            ctx->packets_processed++;
        }
    }

    free(proc_buf);
    LOG_INFO("Process thread stopped (%llu packets processed)",
             (unsigned long long)ctx->packets_processed);
    return NULL;
}

/*
 * Status thread - periodically monitors signal quality
 */
static void *status_thread_func(void *arg)
{
    capture_context_t *ctx = (capture_context_t *)arg;
    (void)ctx;  /* Will use bridge for signal monitoring */

    LOG_INFO("Status thread started");

    while (ctx->should_run) {
        /* Report capture stats every 5 seconds */
        LOG_INFO("Capture stats: %llu bytes, %llu packets, %u errors",
                 (unsigned long long)ctx->bytes_received,
                 (unsigned long long)ctx->packets_processed,
                 ctx->errors);
        br_user_delay(5000);
    }

    LOG_INFO("Status thread stopped");
    return NULL;
}

hdtvmate_error_t capture_start(capture_context_t *ctx, usb_device_t *usb,
                                it9300_device_t *bridge,
                                capture_callback_t callback, void *user_data)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->usb = usb;
    ctx->bridge = bridge;
    ctx->callback = callback;
    ctx->callback_data = user_data;

    /* Configure IT9300 TS output before starting bulk reads. Without this
     * the EP 0x84 endpoint stays stalled and bulk_read returns
     * LIBUSB_ERROR_PIPE on every attempt. */
    it9300_config_output(bridge);
    it9300_enable_ts_port(bridge, 0);

    /* Initialize ring buffer */
    hdtvmate_error_t ret = circular_buffer_init(&ctx->ring_buffer, RING_BUFFER_SIZE);
    if (ret != HDTVMATE_OK) return ret;

    ctx->should_run = true;
    ctx->running = true;

    /* Start threads */
    pthread_create(&ctx->capture_thread, NULL, capture_thread_func, ctx);
    pthread_create(&ctx->process_thread, NULL, process_thread_func, ctx);
    pthread_create(&ctx->status_thread, NULL, status_thread_func, ctx);

    LOG_INFO("Capture started (3 threads)");
    return HDTVMATE_OK;
}

hdtvmate_error_t capture_stop(capture_context_t *ctx)
{
    if (!ctx->running) return HDTVMATE_OK;

    LOG_INFO("Stopping capture...");
    ctx->should_run = false;

    pthread_join(ctx->capture_thread, NULL);
    pthread_join(ctx->process_thread, NULL);
    pthread_join(ctx->status_thread, NULL);

    circular_buffer_free(&ctx->ring_buffer);
    ctx->running = false;

    LOG_INFO("Capture stopped");
    return HDTVMATE_OK;
}

void capture_get_stats(capture_context_t *ctx, uint64_t *bytes, uint64_t *packets)
{
    if (bytes) *bytes = ctx->bytes_received;
    if (packets) *packets = ctx->packets_processed;
}
