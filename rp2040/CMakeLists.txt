cmake_minimum_required(VERSION 3.12)

# The RP2040 on the deck is wired like a Pico as far as the SDK cares: the same
# ABM8-272-T3 12 MHz crystal and a 2 MB W25Q16JV. Pin usage lives in include/board_bugslayer_config.h.
set(PICO_BOARD pico CACHE STRING "Board type")

include(pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/freertos)
include(FreeRTOS_Kernel_import.cmake)

project(bugslayer_probe C CXX ASM)

pico_sdk_init()

if (${PICO_SDK_VERSION_MAJOR} LESS 2)
	message(SEND_ERROR "Version 2 of the Pico SDK is required to compile this project. Please update your installation at ${PICO_SDK_PATH}")
endif ()

add_executable(bugslayer_probe
        src/probe_config.c
        src/main.c
        src/usb_descriptors.c
        src/probe.c
        src/cdc_uart.c
        src/get_serial.c
        src/sw_dp_pio.c
        src/tusb_edpt_handler.c
        src/cdc_shared.c
)

target_sources(bugslayer_probe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/JTAG_DP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP_vendor.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/SWO.c
        )

target_include_directories(bugslayer_probe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Include/
        CMSIS_DAP/CMSIS/Core/Include/
        include/
        src/
        )

target_compile_options(bugslayer_probe PRIVATE -Wall)

pico_generate_pio_header(bugslayer_probe ${CMAKE_CURRENT_LIST_DIR}/src/probe.pio)
pico_generate_pio_header(bugslayer_probe ${CMAKE_CURRENT_LIST_DIR}/src/probe_oen.pio)

target_compile_definitions(bugslayer_probe PRIVATE
	PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)

target_link_libraries(bugslayer_probe PRIVATE
        pico_multicore
        pico_stdlib
        pico_unique_id
        tinyusb_device
        tinyusb_board
        hardware_pio
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap1
)

# The default stdio UART would claim GP0/GP1 (LED_USB and TP1) from tinyusb's board_init()
pico_enable_stdio_uart(bugslayer_probe 0)
pico_enable_stdio_usb(bugslayer_probe 0)

pico_set_binary_type(bugslayer_probe copy_to_ram)

pico_add_extra_outputs(bugslayer_probe)
