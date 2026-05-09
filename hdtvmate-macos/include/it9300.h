#ifndef IT9300_H
#define IT9300_H

#include "hdtvmate.h"
#include "usb_device.h"

/*
 * ITE IT9300 USB Bridge Driver
 *
 * The IT9300 is a USB bridge chip that provides:
 * - USB 2.0 High-Speed interface to the host
 * - I2C master for communicating with demodulator chips (CXD6801)
 * - TS (Transport Stream) mux/demux
 * - PID filtering in hardware
 * - EEPROM access for device configuration
 * - Firmware loading
 *
 * Command protocol (from Linux kernel af9035.c):
 * All commands use bulk transfer with the following frame format:
 *
 *   [LEN_LO] [LEN_HI] [SEQ] [CMD] [DATA...] [CHECKSUM]
 *
 * Response:
 *   [LEN_LO] [LEN_HI] [SEQ] [CMD] [STATUS] [DATA...] [CHECKSUM]
 */

/* Endeavour command codes (from BrCmd_writeRegisters/readRegisters disassembly)
 * CMD is 16-bit: CMD_HI = (processor << 4), CMD_LO = operation
 * For LINK processor: CMD = 0x0000 (read), 0x0001 (write)
 */
#define IT9300_CMD_REG_RD       0x00    /* Endeavour register read (LINK) */
#define IT9300_CMD_REG_WR       0x01    /* Endeavour register write (LINK) */
#define IT9300_CMD_FW_DL        0x21    /* Firmware download */
#define IT9300_CMD_FW_DL_BEGIN  0x24    /* Firmware download begin */
#define IT9300_CMD_FW_DL_END   0x25    /* Firmware download end */
#define IT9300_CMD_FW_BOOT      0x23    /* Firmware boot/reboot */
#define IT9300_CMD_QUERYINFO    0x22    /* Query device info */
#define IT9300_CMD_IR_GET       0x18    /* Get IR code */
#define IT9300_CMD_FW_SCATTER_WR 0x29  /* Scatter write */
#define IT9300_CMD_GENERIC_I2C_RD 0x2A  /* Endeavour generic I2C read */
#define IT9300_CMD_GENERIC_I2C_WR 0x2B  /* Endeavour generic I2C write */

/* IT9300 Processor IDs */
#define IT9300_PROCESSOR_LINK   0x00
#define IT9300_PROCESSOR_OFDM   0x01
#define IT9300_PROCESSOR_SEC    0x02    /* Security processor */

/* IT9300 register addresses (commonly used) */
#define IT9300_REG_CHIP_VERSION     0x1222
#define IT9300_REG_FW_VERSION       0x0083
#define IT9300_REG_LINK_VERSION     0x1222

/* TS port enable register — confirmed via disassembly of
 * IT9300_enableTsPort @ 0x18d880 in liba3_phy_sony.so:
 *   w8 = 0xDA4C (base);  w8 += offset_from_struct[0x30]
 *   IT9300_writeRegister(handle, port_idx, w8, 1) → enable
 *   IT9300_writeRegister(handle, port_idx, w8, 0) → disable
 *
 * For port=0, channel=0 the offset is 0, so the actual reg is 0xDA4C.
 * Our previous guess (0xD827) was wrong — that register doesn't gate
 * EP 0x84 streaming and writing it produces no effect. */
#define IT9300_REG_TS_OUTPUT_MODE   0xDA4C
#define IT9300_REG_TS_PARALLEL      0xD828
#define IT9300_REG_TS_SERIAL        0xD829
#define IT9300_REG_PID_FILTER_CTRL  0xD860

/* IT9300 EEPROM base addresses */
#define IT9300_EEPROM_BASE          0x499C
#define IT9300_EEPROM_TSMODE        0x499E
#define IT9300_EEPROM_TUNERTYPE     0x49AC

/* Max USB transfer sizes */
#define IT9300_USB_MAX_WRITE        63    /* Max command data (excl. header) */
#define IT9300_USB_MAX_READ         63
#define IT9300_USB_CMD_HEADER_SIZE  4     /* len_lo, len_hi, seq, cmd */
#define IT9300_USB_CMD_CHECKSUM     1     /* checksum byte */
#define IT9300_USB_BUF_SIZE         256

/* Firmware structure (extracted from binary) */
typedef struct {
    const uint8_t  *codes;
    uint32_t        codes_len;
    const uint8_t  *segments;
    uint32_t        segments_len;
    const uint8_t  *partitions;
    uint32_t        partitions_len;
    const uint8_t  *script_sets;
    uint32_t        script_sets_len;
    const uint8_t  *scripts;
    uint32_t        scripts_len;
} it9300_firmware_t;

/* EEPROM device configuration */
typedef struct {
    uint8_t  chip_type;
    uint8_t  rx_device_count;
    uint8_t  ts_mode;           /* TS output mode */
    uint8_t  tuner_type[4];     /* Per-RX tuner type IDs */
    uint8_t  demod_type[4];     /* Per-RX demod type IDs */
    uint8_t  i2c_addr_tuner[4]; /* I2C addresses for tuners */
    uint8_t  i2c_addr_demod[4]; /* I2C addresses for demods */
    uint16_t board_id;
    uint32_t fw_version;
} it9300_eeprom_config_t;

/* IT9300 device context */
typedef struct {
    usb_device_t          *usb;
    it9300_eeprom_config_t eeprom;
    it9300_firmware_t      firmware;
    uint8_t                cmd_seq;     /* Command sequence counter */
    uint32_t               fw_version;
    bool                   initialized;
} it9300_device_t;

/* Initialization */
hdtvmate_error_t it9300_initialize(it9300_device_t *dev, usb_device_t *usb);
hdtvmate_error_t it9300_load_firmware(it9300_device_t *dev);
hdtvmate_error_t it9300_reboot(it9300_device_t *dev);
hdtvmate_error_t it9300_get_firmware_version(it9300_device_t *dev, uint32_t *version);

/* EEPROM */
hdtvmate_error_t it9300_read_eeprom_config(it9300_device_t *dev);

/* Register access */
hdtvmate_error_t it9300_read_register(it9300_device_t *dev, uint8_t processor,
                                       uint32_t addr, uint8_t *value);
hdtvmate_error_t it9300_write_register(it9300_device_t *dev, uint8_t processor,
                                        uint32_t addr, uint8_t value);
hdtvmate_error_t it9300_read_registers(it9300_device_t *dev, uint8_t processor,
                                        uint32_t addr, uint8_t *values, uint8_t len);
hdtvmate_error_t it9300_write_registers(it9300_device_t *dev, uint8_t processor,
                                         uint32_t addr, const uint8_t *values, uint8_t len);

/* I2C passthrough (for demodulator communication) */
hdtvmate_error_t it9300_i2c_read(it9300_device_t *dev, uint8_t i2c_addr,
                                  uint8_t *data, uint8_t len);
hdtvmate_error_t it9300_i2c_write(it9300_device_t *dev, uint8_t i2c_addr,
                                   const uint8_t *data, uint8_t len);
hdtvmate_error_t it9300_read_generic_registers(it9300_device_t *dev,
                                                uint8_t chip_idx,
                                                uint8_t i2c_addr, uint8_t i2c_bus,
                                                uint8_t *data, uint8_t len);
hdtvmate_error_t it9300_write_generic_registers(it9300_device_t *dev,
                                                 uint8_t chip_idx,
                                                 uint8_t i2c_addr, uint8_t i2c_bus,
                                                 const uint8_t *data, uint8_t len);

/* TS port configuration */
hdtvmate_error_t it9300_enable_ts_port(it9300_device_t *dev, uint8_t port);
hdtvmate_error_t it9300_disable_ts_port(it9300_device_t *dev, uint8_t port);
hdtvmate_error_t it9300_config_output(it9300_device_t *dev);
hdtvmate_error_t it9300_set_out_ts_type(it9300_device_t *dev);

/* PID filtering */
hdtvmate_error_t it9300_enable_pid_filter(it9300_device_t *dev, uint8_t port,
                                           uint8_t index, uint16_t pid);
hdtvmate_error_t it9300_disable_pid_filter(it9300_device_t *dev, uint8_t port,
                                            uint8_t index);
hdtvmate_error_t it9300_reset_pid_filter(it9300_device_t *dev, uint8_t port);

/* Cleanup */
void it9300_deinit(it9300_device_t *dev);

#endif /* IT9300_H */
