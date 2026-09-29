cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD bugslayer_deck CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(bugslayer_deck_rp2350 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(bugslayer_deck
    src/main.c
    src/fx2_link.c
)

pico_generate_pio_header(bugslayer_deck ${CMAKE_CURRENT_LIST_DIR}/pio/fx2_write.pio)

target_include_directories(bugslayer_deck PRIVATE src)

target_link_libraries(bugslayer_deck
    pico_stdlib
    pico_unique_id
    hardware_pio
    hardware_dma
)

# Control plane on the RP2350's own USB; nothing goes to a UART.
pico_enable_stdio_usb(bugslayer_deck 1)
pico_enable_stdio_uart(bugslayer_deck 0)

# USB identity. The serial string is already the flash unique ID, which is what
# the host uses to pair this device with the deck's FX2 (docs/protocol.md).
target_compile_definitions(bugslayer_deck PRIVATE
    USBD_VID=0x35F0
    USBD_PID=0xDB12
    USBD_MANUFACTURER="Bitcraze AB"
    USBD_PRODUCT="Bugslayer deck ctrl"  # the SDK caps strings at 19 chars
)

pico_add_extra_outputs(bugslayer_deck)
