cmake_minimum_required(VERSION 3.20)
project(hdtvmate VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find libusb
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# Common include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${LIBUSB_INCLUDE_DIRS}
)

link_directories(${LIBUSB_LIBRARY_DIRS})

# --- Core driver library ---
add_library(hdtvmate_core STATIC
    src/usb/usb_device.c
    src/bridge/br_user.c
    src/bridge/br_cmd.c
    src/bridge/it9300.c
    src/demod/cxd6801_i2c_ite.c
    src/demod/cxd6801.c
    src/demod/cxd6801_atsc3.c
    src/demod/cxd6801_atecc.c
    src/demod/cxd6801_monitor.c
    src/demod/ascot3.c
    src/scan/frequency_table.c
    src/scan/channel_scan.c
    src/capture/circular_buffer.c
    src/capture/capture_thread.c
    src/output/udp_output.c
    src/output/file_output.c
)

target_link_libraries(hdtvmate_core ${LIBUSB_LIBRARIES} pthread)

# --- CLI tool: device detection ---
add_executable(hdtvmate_detect src/tools/detect.c)
target_link_libraries(hdtvmate_detect hdtvmate_core)

# --- CLI tool: full initialization ---
add_executable(hdtvmate_init src/tools/init_test.c)
target_link_libraries(hdtvmate_init hdtvmate_core)

# --- CLI tool: channel scan ---
add_executable(hdtvmate_scan src/tools/scan.c)
target_link_libraries(hdtvmate_scan hdtvmate_core)

# --- CLI tool: tune and capture ---
add_executable(hdtvmate_tune src/tools/tune.c)
target_link_libraries(hdtvmate_tune hdtvmate_core)

# --- CLI tool: tune test (no capture, just lock check) ---
add_executable(hdtvmate_tune_test src/tools/tune_test.c)
target_link_libraries(hdtvmate_tune_test hdtvmate_core)

# --- CLI tool: probe SLVR i2c=0x98 across buses + alternate addrs ---
add_executable(slvr_bus_probe src/tools/slvr_bus_probe.c)
target_link_libraries(slvr_bus_probe hdtvmate_core)

add_executable(slvr_replay_sony src/tools/slvr_replay_sony.c)
target_link_libraries(slvr_replay_sony hdtvmate_core)

add_executable(slvr_no_init src/tools/slvr_no_init.c)
target_link_libraries(slvr_no_init hdtvmate_core)

# --- CLI tool: full reset scan ---
add_executable(hdtvmate_resetscan src/tools/full_reset_scan.c)
target_link_libraries(hdtvmate_resetscan hdtvmate_core)

# --- CLI tool: GPIO reset search ---
add_executable(hdtvmate_gpio src/tools/gpio_reset.c)
target_link_libraries(hdtvmate_gpio hdtvmate_core)

# --- CLI tool: EEPROM dump ---
add_executable(hdtvmate_eeprom src/tools/eeprom_dump.c)
target_link_libraries(hdtvmate_eeprom hdtvmate_core)

# --- Extended brute force (debug tool for stuck-repeater recovery) ---
add_executable(extended_repeater_brute src/tools/extended_repeater_brute.c)
target_link_libraries(extended_repeater_brute hdtvmate_core)

# --- Post-Android probe (after Android scan in UTM hands chip back) ---
add_executable(post_android_test src/tools/post_android_test.c)
target_link_libraries(post_android_test hdtvmate_core)

# --- No-reset probe (skip GPIO reset to preserve Android's chip state) ---
add_executable(no_reset_probe src/tools/no_reset_probe.c)
target_link_libraries(no_reset_probe hdtvmate_core)

# --- Channel sweep 14-30 ---
add_executable(scan_14_30 src/tools/scan_14_30.c)
target_link_libraries(scan_14_30 hdtvmate_core)

# --- Full ATSC scan (channels 2-51, both modes) ---
add_executable(scan_full src/tools/scan_full.c)
target_link_libraries(scan_full hdtvmate_core)

# --- Extended lock status polling (verifies sync_stat, ALP, SNR over time) ---
add_executable(test_lock_extended src/tools/test_lock_extended.c)
target_link_libraries(test_lock_extended hdtvmate_core)

# --- 30-second sync_stat distribution histogram ---
add_executable(long_lock_test src/tools/long_lock_test.c)
target_link_libraries(long_lock_test hdtvmate_core)

# --- Real-time SNR / sync_stat monitor for antenna tuning ---
add_executable(snr_monitor src/tools/snr_monitor.c)
target_link_libraries(snr_monitor hdtvmate_core)

# Compiler flags
target_compile_options(hdtvmate_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

