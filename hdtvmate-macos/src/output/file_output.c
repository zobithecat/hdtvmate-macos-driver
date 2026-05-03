#include "capture.h"
#include <stdio.h>
#include <string.h>

/*
 * File output - writes captured TS data to a file
 */

static FILE *g_output_file = NULL;

static void file_callback(const uint8_t *data, size_t len, void *user_data)
{
    FILE *f = (FILE *)user_data;
    fwrite(data, 1, len, f);
}

hdtvmate_error_t output_to_file(const char *filename, capture_context_t *ctx)
{
    g_output_file = fopen(filename, "wb");
    if (!g_output_file) {
        LOG_ERR("Failed to open output file: %s", filename);
        return HDTVMATE_ERR_USB_INIT;
    }

    ctx->callback = file_callback;
    ctx->callback_data = g_output_file;

    LOG_INFO("File output configured: %s", filename);
    return HDTVMATE_OK;
}
