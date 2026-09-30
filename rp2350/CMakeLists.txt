cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD bugslayer_deck CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(bugslayer_deck_rp2350 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Firmware version, reported by `ver`. Release builds pass -DFW_VERSION=<tag
# without the v>; otherwise `git describe` at configure time (re-run cmake to
# refresh it).
if (NOT FW_VERSION)
    execute_process(COMMAND git describe --tags --match "v[0-9]*" --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE FW_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (NOT FW_VERSION MATCHES "^v[0-9]")
        set(FW_VERSION "0.0.0-g${FW_VERSION}")   # no release tag yet
    endif ()
    string(REGEX REPLACE "^v" "" FW_VERSION "${FW_VERSION}")
endif ()
message(STATUS "Bugslayer deck RP2350 firmware ${FW_VERSION}")

add_executable(bugslayer_deck
    src/main.c
    src/fx2_link.c
    src/fx2_boot.c
    src/capture.c
    src/i2c_master.c
    src/sampler.c
    src/usb_stream.c
    src/usb_descriptors.c
    src/uart_bridge.c
)

pico_generate_pio_header(bugslayer_deck ${CMAKE_CURRENT_LIST_DIR}/pio/fx2_write.pio)
pico_generate_pio_header(bugslayer_deck ${CMAKE_CURRENT_LIST_DIR}/pio/sampler.pio)
pico_generate_pio_header(bugslayer_deck ${CMAKE_CURRENT_LIST_DIR}/pio/sck8.pio)

target_include_directories(bugslayer_deck PRIVATE src)
target_compile_definitions(bugslayer_deck PRIVATE FW_VERSION="${FW_VERSION}")

target_link_libraries(bugslayer_deck
    pico_stdlib
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_i2c
    pico_i2c_slave
    pico_rand
    # Our own TinyUSB configuration and descriptors (src/tusb_config.h,
    # src/usb_descriptors.c). pico_stdio_usb sees tinyusb_device and runs on CDC 0.
    tinyusb_device
    tinyusb_board
)

# Control plane on the RP2350's own USB; nothing goes to a UART.
pico_enable_stdio_usb(bugslayer_deck 1)
pico_enable_stdio_uart(bugslayer_deck 0)

# USB identity and descriptors live in src/usb_descriptors.c: 35F0:DB12, serial
# = flash unique ID, which the host uses to pair this device with the FX2.

pico_add_extra_outputs(bugslayer_deck)
