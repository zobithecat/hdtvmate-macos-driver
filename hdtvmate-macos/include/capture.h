#ifndef CAPTURE_H
#define CAPTURE_H

#include "hdtvmate.h"
#include "usb_device.h"
#include "it9300.h"
#include <pthread.h>

/* Circular buffer for USB TS data */
typedef struct {
    uint8_t  *buffer;
    size_t    capacity;
    size_t    head;
    size_t    tail;
    size_t    count;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} circular_buffer_t;

/* Capture data callback */
typedef void (*capture_callback_t)(const uint8_t *data, size_t len, void *user_data);

/* Capture thread context */
typedef struct {
    usb_device_t       *usb;
    it9300_device_t    *bridge;
    circular_buffer_t   ring_buffer;
    capture_callback_t  callback;
    void               *callback_data;
    pthread_t           capture_thread;
    pthread_t           process_thread;
    pthread_t           status_thread;
    volatile bool       should_run;
    bool                running;
    /* Stats */
    uint64_t            bytes_received;
    uint64_t            packets_processed;
    uint32_t            errors;
} capture_context_t;

/* Circular buffer */
hdtvmate_error_t circular_buffer_init(circular_buffer_t *buf, size_t capacity);
size_t circular_buffer_write(circular_buffer_t *buf, const uint8_t *data, size_t len);
size_t circular_buffer_read(circular_buffer_t *buf, uint8_t *data, size_t len);
size_t circular_buffer_available(circular_buffer_t *buf);
void circular_buffer_free(circular_buffer_t *buf);

/* Capture */
hdtvmate_error_t capture_start(capture_context_t *ctx, usb_device_t *usb,
                                it9300_device_t *bridge,
                                capture_callback_t callback, void *user_data);
hdtvmate_error_t capture_stop(capture_context_t *ctx);
void capture_get_stats(capture_context_t *ctx, uint64_t *bytes, uint64_t *packets);

/* Output helpers */
hdtvmate_error_t output_to_file(const char *filename, capture_context_t *ctx);
hdtvmate_error_t output_to_udp(const char *addr, uint16_t port, capture_context_t *ctx);

#endif /* CAPTURE_H */
