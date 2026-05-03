#ifndef HDTVMATE_H
#define HDTVMATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Error codes */
typedef enum {
    HDTVMATE_OK = 0,
    HDTVMATE_ERR_USB_INIT = -1,
    HDTVMATE_ERR_DEVICE_NOT_FOUND = -2,
    HDTVMATE_ERR_USB_OPEN = -3,
    HDTVMATE_ERR_USB_CLAIM = -4,
    HDTVMATE_ERR_USB_TRANSFER = -5,
    HDTVMATE_ERR_FIRMWARE = -6,
    HDTVMATE_ERR_BRIDGE_INIT = -7,
    HDTVMATE_ERR_DEMOD_INIT = -8,
    HDTVMATE_ERR_TUNE = -9,
    HDTVMATE_ERR_NO_LOCK = -10,
    HDTVMATE_ERR_TIMEOUT = -11,
    HDTVMATE_ERR_INVALID_PARAM = -12,
    HDTVMATE_ERR_CHECKSUM = -13,
    HDTVMATE_ERR_NOT_READY = -14,
} hdtvmate_error_t;

/* Logging */
typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
    LOG_TRACE = 4,
} log_level_t;

extern log_level_t g_log_level;

#define LOG(level, fmt, ...) do { \
    if ((level) <= g_log_level) { \
        const char *_lvl[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"}; \
        fprintf(stderr, "[%s] %s:%d: " fmt "\n", _lvl[level], __func__, __LINE__, ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_ERR(fmt, ...)   LOG(LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...)   LOG(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_TRC(fmt, ...)   LOG(LOG_TRACE, fmt, ##__VA_ARGS__)

/* ATSC broadcast standards */
typedef enum {
    BROADCAST_ATSC3 = 0,  /* ATSC 3.0 (NextGen TV) */
    BROADCAST_ATSC1 = 1,  /* ATSC 1.0 (legacy) */
    BROADCAST_J83B  = 2,  /* J.83B (QAM cable) */
} broadcast_standard_t;

/* Signal statistics */
typedef struct {
    bool     locked;
    int32_t  snr_db_x100;     /* SNR in dB * 100 */
    int32_t  rf_level_dbm;    /* RF level in dBm */
    uint32_t ber_numerator;
    uint32_t ber_denominator;
    uint32_t per;              /* Packet error rate */
    uint8_t  modulation;
    uint8_t  fec_rate;
} signal_stats_t;

/* Channel info from scan */
typedef struct {
    uint32_t frequency_khz;
    uint8_t  channel_number;
    broadcast_standard_t standard;
    signal_stats_t stats;
    uint8_t  num_plps;
    uint8_t  plp_ids[64];
    char     name[64];
} channel_info_t;

#endif /* HDTVMATE_H */
