#include "capture.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/*
 * UDP output - sends captured TS data via UDP for playback with VLC/mpv
 *
 * Usage:
 *   ./hdtvmate_tune 473000 --udp 127.0.0.1:1234
 *   vlc udp://@:1234
 *   # or: mpv udp://127.0.0.1:1234
 */

typedef struct {
    int sock_fd;
    struct sockaddr_in dest_addr;
} udp_output_ctx_t;

static udp_output_ctx_t g_udp_ctx;

static void udp_callback(const uint8_t *data, size_t len, void *user_data)
{
    udp_output_ctx_t *ctx = (udp_output_ctx_t *)user_data;

    /* Send in chunks that fit in UDP (max ~65507 bytes, but use 1316 for TS) */
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > 1316) ? 1316 : (len - offset);
        sendto(ctx->sock_fd, data + offset, chunk, 0,
               (struct sockaddr *)&ctx->dest_addr, sizeof(ctx->dest_addr));
        offset += chunk;
    }
}

hdtvmate_error_t output_to_udp(const char *addr, uint16_t port, capture_context_t *ctx)
{
    /* Create UDP socket */
    g_udp_ctx.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_ctx.sock_fd < 0) {
        LOG_ERR("Failed to create UDP socket");
        return HDTVMATE_ERR_USB_INIT;
    }

    memset(&g_udp_ctx.dest_addr, 0, sizeof(g_udp_ctx.dest_addr));
    g_udp_ctx.dest_addr.sin_family = AF_INET;
    g_udp_ctx.dest_addr.sin_port = htons(port);
    inet_pton(AF_INET, addr, &g_udp_ctx.dest_addr.sin_addr);

    /* Set callback */
    ctx->callback = udp_callback;
    ctx->callback_data = &g_udp_ctx;

    LOG_INFO("UDP output configured: %s:%d", addr, port);
    return HDTVMATE_OK;
}
