#pragma once

#include "cdc_shared.h"

void cdc_uart_poll_port_switcher(struct CDCUartInstance_t* uart);
void cdc_uart_thread(void *ptr);
void cdc_uart_init(struct CDCUartInstance_t* uart, const struct UartProfile_t* profile);
void cdc_uart_deinit(struct CDCUartInstance_t* uart);
bool cdc_uart_task(struct CDCUartInstance_t* uart);
void cdc_uart_line_coding_cb(struct CDCUartInstance_t* uart, uint8_t itf, cdc_line_coding_t const* line_coding);
void cdc_uart_line_state_cb(struct CDCUartInstance_t* uart, uint8_t itf, bool dtr, bool rts);
void cdc_uart_activate_iff_profiles( uint32_t profileId );    
void cdc_uart_core2(void* ptr);
void cdc_handle_profile_switch(struct CDCUartInstance_t* uart);