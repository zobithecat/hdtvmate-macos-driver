#ifndef CHANNEL_SCAN_H
#define CHANNEL_SCAN_H

#include "hdtvmate.h"
#include "cxd6801.h"

/* ATSC frequency table entry */
typedef struct {
    uint8_t  channel_number;    /* RF channel number (e.g., 14-51 for UHF) */
    uint32_t frequency_khz;     /* Center frequency in kHz */
    const char *band;           /* "VHF-Lo", "VHF-Hi", "UHF" */
} atsc_frequency_t;

/* Scan callback - called for each channel found */
typedef void (*scan_callback_t)(const channel_info_t *channel, void *user_data);

/* Scan progress callback */
typedef void (*scan_progress_t)(uint32_t current, uint32_t total,
                                 uint32_t freq_khz, void *user_data);

/* Scan configuration */
typedef struct {
    broadcast_standard_t  standard;    /* ATSC3, ATSC1, or both */
    bool                  scan_atsc3;
    bool                  scan_atsc1;
    uint32_t              lock_timeout_ms;  /* Max time to wait for lock per freq */
    scan_callback_t       on_channel_found;
    scan_progress_t       on_progress;
    void                 *user_data;
} scan_config_t;

/* Scan results */
typedef struct {
    channel_info_t  channels[128];
    uint32_t        count;
} scan_results_t;

/* Get ATSC frequency table */
const atsc_frequency_t *frequency_table_get(uint32_t *count);

/* Get frequency for a specific channel number */
uint32_t frequency_for_channel(uint8_t channel_number);

/* Perform full channel scan */
hdtvmate_error_t channel_scan(cxd6801_device_t *dev, const scan_config_t *config,
                               scan_results_t *results);

/* Scan a single frequency */
hdtvmate_error_t channel_scan_single(cxd6801_device_t *dev, uint32_t frequency_khz,
                                      broadcast_standard_t std, channel_info_t *info);

#endif /* CHANNEL_SCAN_H */
