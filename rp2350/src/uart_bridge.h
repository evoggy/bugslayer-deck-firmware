// Crazyflie UART1/UART2 on the expansion port <-> the deck's CDC ports 1/2.
//
// Standalone only: the deck stands in for the Crazyflie, so the hardware UART
// drives the TX pin and reads the RX pin, exactly as the CF's STM32 would.
// Baud rate and format follow the host's line coding on each CDC port.
// Off by default; the pins are then plain inputs like every other CF signal.
// Capture keeps seeing both lines either way (pad inputs are shared).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define UART_BRIDGE_PORTS 2      // index 0 = UART1 (uart0), 1 = UART2 (uart1)

void     uart_bridge_init(void);
void     uart_bridge_set(uint32_t port, bool on);
bool     uart_bridge_on(uint32_t port);
uint32_t uart_bridge_baud(uint32_t port);
uint32_t uart_bridge_dropped(uint32_t port);   // RX bytes lost: ring full or FIFO overrun
void     uart_bridge_poll(void);                // move bytes; call from the main loop
