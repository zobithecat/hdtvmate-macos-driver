#include "channel_scan.h"
#include <stdio.h>
#include <string.h>

/*
 * channel_scan.c - Full channel scan implementation
 *
 * Scans the ATSC frequency table for active channels.
 * For each frequency:
 * 1. Tune demodulator
 * 2. Wait for lock (with timeout)
 * 3. If locked, read signal info and PLP list
 * 4. Report channel via callback
 */

extern void br_user_delay(uint32_t ms);

hdtvmate_error_t channel_scan_single(cxd6801_device_t *dev, uint32_t frequency_khz,
                                      broadcast_standard_t std, channel_info_t *info)
{
    hdtvmate_error_t ret;

    memset(info, 0, sizeof(*info));
    info->frequency_khz = frequency_khz;
    info->standard = std;

    /* Try to acquire channel */
    ret = cxd6801_acquire_channel(dev, frequency_khz, std);
    if (ret != HDTVMATE_OK) {
        return ret;  /* No lock = no channel */
    }

    /* Channel acquired - gather signal info */
    info->stats.locked = true;

    /* Get signal statistics */
    cxd6801_get_stats(dev, &info->stats);

    /* For ATSC 3.0, get PLP list */
    if (std == BROADCAST_ATSC3) {
        cxd6801_monitor_plp_list(dev, info->plp_ids, &info->num_plps);
    }

    /* Return to sleep for next frequency */
    cxd6801_sleep(dev);

    return HDTVMATE_OK;
}

hdtvmate_error_t channel_scan(cxd6801_device_t *dev, const scan_config_t *config,
                               scan_results_t *results)
{
    uint32_t freq_count;
    const atsc_frequency_t *freq_table = frequency_table_get(&freq_count);

    memset(results, 0, sizeof(*results));

    LOG_INFO("Starting channel scan: %u frequencies", freq_count);

    /* Determine lock timeout */
    uint32_t timeout = config->lock_timeout_ms;
    if (timeout == 0) timeout = 2000;  /* Default 2 seconds */

    uint32_t progress = 0;
    uint32_t total = freq_count;

    /* Count total iterations based on standards to scan */
    if (config->scan_atsc3 && config->scan_atsc1) {
        total = freq_count * 2;
    }

    for (uint32_t i = 0; i < freq_count; i++) {
        uint32_t freq = freq_table[i].frequency_khz;
        uint8_t ch = freq_table[i].channel_number;

        /* Scan ATSC 3.0 */
        if (config->scan_atsc3) {
            progress++;
            if (config->on_progress) {
                config->on_progress(progress, total, freq, config->user_data);
            }

            LOG_INFO("Scanning CH %d (%u kHz) ATSC 3.0...", ch, freq);

            channel_info_t info;
            hdtvmate_error_t ret = channel_scan_single(dev, freq, BROADCAST_ATSC3, &info);

            if (ret == HDTVMATE_OK && info.stats.locked) {
                info.channel_number = ch;
                snprintf(info.name, sizeof(info.name), "CH%d-ATSC3", ch);

                LOG_INFO("  FOUND: CH %d ATSC3 - SNR=%d.%02d dB, PLPs=%d",
                         ch, info.stats.snr_db_x100 / 100,
                         info.stats.snr_db_x100 % 100, info.num_plps);

                if (results->count < 128) {
                    results->channels[results->count++] = info;
                }

                if (config->on_channel_found) {
                    config->on_channel_found(&info, config->user_data);
                }
            }
        }

        /* Scan ATSC 1.0 */
        if (config->scan_atsc1) {
            progress++;
            if (config->on_progress) {
                config->on_progress(progress, total, freq, config->user_data);
            }

            LOG_INFO("Scanning CH %d (%u kHz) ATSC 1.0...", ch, freq);

            channel_info_t info;
            hdtvmate_error_t ret = channel_scan_single(dev, freq, BROADCAST_ATSC1, &info);

            if (ret == HDTVMATE_OK && info.stats.locked) {
                info.channel_number = ch;
                snprintf(info.name, sizeof(info.name), "CH%d-ATSC1", ch);

                LOG_INFO("  FOUND: CH %d ATSC1 - locked", ch);

                if (results->count < 128) {
                    results->channels[results->count++] = info;
                }

                if (config->on_channel_found) {
                    config->on_channel_found(&info, config->user_data);
                }
            }
        }
    }

    LOG_INFO("Scan complete: %u channels found", results->count);
    return HDTVMATE_OK;
}
