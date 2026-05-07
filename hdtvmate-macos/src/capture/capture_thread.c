#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

/*
 * capture_thread.c - USB async bulk capture using libusb async transfers
 *
 * Mirrors SonyPHYAndroid::readFromUsbDemodEndpointRxTs (@ 0x7bc84 in
 * liba3_phy_sony.so) which uses async transfers — IT9300 firmware needs
 * multiple URBs in flight before it starts pushing TS data, so the
 * previous synchronous bulk_read approach got 0 bytes.
 *
 * Pattern (matches Linux af9035.c / dvb-usb-v2):
 *   1. Allocate N libusb_transfer structs + buffers
 *   2. libusb_fill_bulk_transfer + libusb_submit_transfer for each
 *   3. Event loop: libusb_handle_events_timeout
 *   4. Callback fires when data arrives → write to ring + re-submit
 */

/* Per-URB transfer buffer size — Frida-captured Sony Android app value.
 * Sony submits libusb_submit_transfer with len=32768 (32 KB = 0x8000)
 * on EP 0x84. Linux af9035 uses 87*188=16356 (~16 KB) but for ITE/Sony
 * combo the firmware seems to expect the larger 32 KB buffers — our
 * earlier 16 KB attempts got 0 bytes / all timeouts. */
#define URB_BUF_SIZE  (32 * 1024)

/* Process thread buffer (drains ring into callback) */
#define PROC_BUF_SIZE  (128 * 1024)

/* Number of URBs in flight — Sony submits 8 simultaneously per Frida
 * trace. Total in-flight buffers: 8 * 32 KB = 256 KB. */
#define NUM_URBS  8

/* Circular buffer size (8 MB) */
#define RING_BUFFER_SIZE   (8 * 1024 * 1024)

extern void br_user_delay(uint32_t ms);

/* Per-URB context */
typedef struct {
    capture_context_t *ctx;
    struct libusb_transfer *transfer;
    uint8_t *buffer;
} urb_ctx_t;

static void LIBUSB_CALL transfer_cb(struct libusb_transfer *t)
{
    urb_ctx_t *uctx = (urb_ctx_t *)t->user_data;
    capture_context_t *ctx = uctx->ctx;

    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0) {
        circular_buffer_write(&ctx->ring_buffer, t->buffer, t->actual_length);
        ctx->bytes_received += t->actual_length;
    } else if (t->status != LIBUSB_TRANSFER_COMPLETED &&
               t->status != LIBUSB_TRANSFER_TIMED_OUT) {
        ctx->errors++;
        LOG_WARN("URB error status=%d actual=%d", t->status, t->actual_length);
    } else if (t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        LOG_DBG("URB timeout (no data, length=%d)", t->actual_length);
    } else if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        LOG_DBG("URB completed but length=0");
    }

    /* Re-submit if still running */
    if (ctx->should_run) {
        int r = libusb_submit_transfer(t);
        if (r != 0) {
            LOG_ERR("libusb_submit_transfer (resubmit) failed: %d", r);
            ctx->errors++;
        }
    }
}

static void *capture_thread_func(void *arg)
{
    capture_context_t *ctx = (capture_context_t *)arg;
    urb_ctx_t urbs[NUM_URBS];
    struct libusb_device_handle *handle = (struct libusb_device_handle *)ctx->usb->handle;
    uint8_t ep = ctx->usb->endpoints.ep_rx_ts | 0x80;

    LOG_INFO("Async capture thread started (EP 0x%02x, %d URBs x %d bytes)",
             ep, NUM_URBS, URB_BUF_SIZE);

    /* Clear stale halt on EP 0x84 */
    libusb_clear_halt(handle, ep);
    /* libusb_reset_device tested — broke USB state, removed.
     * Sony only calls reset_device in destructor (cleanup), not init. */

    /* Allocate URBs and buffers */
    int allocated = 0;
    for (int i = 0; i < NUM_URBS; i++) {
        urbs[i].ctx = ctx;
        urbs[i].buffer = (uint8_t *)malloc(URB_BUF_SIZE);
        urbs[i].transfer = libusb_alloc_transfer(0);
        if (!urbs[i].buffer || !urbs[i].transfer) {
            LOG_ERR("Failed to allocate URB %d", i);
            goto cleanup;
        }
        libusb_fill_bulk_transfer(urbs[i].transfer, handle, ep,
                                   urbs[i].buffer, URB_BUF_SIZE,
                                   transfer_cb, &urbs[i],
                                   1000 /* timeout ms */);
        allocated++;
    }

    /* Submit all URBs up front — IT9300 firmware needs multiple ready
     * before it starts streaming. */
    int submitted = 0;
    for (int i = 0; i < NUM_URBS; i++) {
        int r = libusb_submit_transfer(urbs[i].transfer);
        if (r != 0) {
            LOG_ERR("libusb_submit_transfer[%d] failed: %d", i, r);
            ctx->errors++;
            break;
        }
        submitted++;
    }
    LOG_INFO("Submitted %d/%d URBs; entering event loop", submitted, NUM_URBS);

    /* Event loop — drives async transfers. transfer_cb auto-resubmits. */
    while (ctx->should_run) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100ms */
        libusb_handle_events_timeout(NULL, &tv);
    }

    /* Cancel all in-flight transfers */
    for (int i = 0; i < submitted; i++) {
        libusb_cancel_transfer(urbs[i].transfer);
    }
    /* Drain remaining events to let cancellations complete */
    for (int i = 0; i < 5; i++) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        libusb_handle_events_timeout(NULL, &tv);
    }

cleanup:
    for (int i = 0; i < allocated; i++) {
        if (urbs[i].transfer) libusb_free_transfer(urbs[i].transfer);
        if (urbs[i].buffer) free(urbs[i].buffer);
    }
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
    uint8_t *proc_buf = (uint8_t *)malloc(PROC_BUF_SIZE);

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

        size_t to_read = (avail > PROC_BUF_SIZE) ? PROC_BUF_SIZE : avail;
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

    /* Enable TS port — pre-enable register sequence + DA4C=1.
     * IT9300_initialize() already did setOutTsType + configOutput
     * (Sony's order embeds those in init), so we only need
     * enable_ts_port here. */
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
