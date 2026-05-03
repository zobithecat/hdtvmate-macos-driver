#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hdtvmate_error_t circular_buffer_init(circular_buffer_t *buf, size_t capacity)
{
    buf->buffer = (uint8_t *)malloc(capacity);
    if (!buf->buffer) {
        LOG_ERR("Failed to allocate circular buffer (%zu bytes)", capacity);
        return HDTVMATE_ERR_USB_INIT;
    }

    buf->capacity = capacity;
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
    pthread_cond_init(&buf->not_full, NULL);

    return HDTVMATE_OK;
}

size_t circular_buffer_write(circular_buffer_t *buf, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&buf->mutex);

    size_t written = 0;
    while (written < len) {
        if (buf->count >= buf->capacity) {
            /* Buffer full - drop oldest data */
            buf->tail = (buf->tail + 1) % buf->capacity;
            buf->count--;
        }

        buf->buffer[buf->head] = data[written];
        buf->head = (buf->head + 1) % buf->capacity;
        buf->count++;
        written++;
    }

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return written;
}

size_t circular_buffer_read(circular_buffer_t *buf, uint8_t *data, size_t len)
{
    pthread_mutex_lock(&buf->mutex);

    /* Wait for data if empty */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;  /* 1 second timeout */

    while (buf->count == 0) {
        int ret = pthread_cond_timedwait(&buf->not_empty, &buf->mutex, &ts);
        if (ret != 0) {
            pthread_mutex_unlock(&buf->mutex);
            return 0;
        }
    }

    size_t to_read = (len < buf->count) ? len : buf->count;
    size_t read_count = 0;

    while (read_count < to_read) {
        data[read_count] = buf->buffer[buf->tail];
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
        read_count++;
    }

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return read_count;
}

size_t circular_buffer_available(circular_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    size_t count = buf->count;
    pthread_mutex_unlock(&buf->mutex);
    return count;
}

void circular_buffer_free(circular_buffer_t *buf)
{
    if (buf->buffer) {
        free(buf->buffer);
        buf->buffer = NULL;
    }
    pthread_mutex_destroy(&buf->mutex);
    pthread_cond_destroy(&buf->not_empty);
    pthread_cond_destroy(&buf->not_full);
}
