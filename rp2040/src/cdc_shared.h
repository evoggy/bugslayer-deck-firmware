/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Picolemon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */
#pragma once

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hardware/pio.h"
#include "pico/util/queue.h"

// Config
#define MAX_UART_DEVICES 2              // Max ITF bound devices
#define MAX_UART_PROFILES 4             // Max UARTS
#define CDC_UART_LED_FLASH_TIME 750     // LED Blink time
#define CDC_BUFFER_SZ 4096

// Non-hw/Vendor code solution allow uart to write special sequence to switch on boot 
// ( not recommended option as can trigger with enough random data )
//#define ENABLE_MAGIC_KEY_SWITCH 1

/** Uart types */
enum EUartType
{
    EUartType_None,
    EUartType_Hardware
};


/** Uart profile */
typedef struct UartProfile_t
{
    uint32_t index;
    const char * name;
    int txPin;
    int rxPin;
    int hwUartId;
    int baud;
    int activityLedPin;    
} UartProfile_t;


/** Uart state for bound CDC device */
typedef struct CDCUartInstance_t
{
    const char* name;
    enum EUartType type;
    bool isConfigured;    
    uint32_t lastToggleButtonDownStartTime;
    uint8_t itf;
    bool isUartInitialised;
    int32_t rxPin;
    int32_t txPin;        
    int32_t baud;
    TickType_t last_wake;
    TickType_t interval;
    volatile TickType_t break_expiry;
    volatile bool timed_break;
    uint8_t tx_buf[32];    
    TaskHandle_t uart_taskHandle;    
    bool was_connected;
    uint cdc_tx_oe;
    int32_t matchKeyStates[4]; // index of last matched uart key
    int32_t defaultProfileId;

    // profile toggle
    const UartProfile_t* currentProfile;
    const UartProfile_t* nextProfile;

    // hw uart
    uart_inst_t *uart;    

    queue_t uartRxQueue;    
    queue_t freeRxQueue;    
    uint32_t bufferOverflow;    

    uint8_t buffer[CDC_BUFFER_SZ];
    volatile uint32_t readingBuffer;
    volatile uint32_t writingBuffer;    

    uint32_t lastActivityTimeRx;
    uint32_t lastActivityTimeTx;
} CDCUartInstance_t;

extern struct CDCUartInstance_t uartDevices[MAX_UART_DEVICES];
extern struct UartProfile_t uartProfiles[MAX_UART_PROFILES];

void process_uart_triggers(struct CDCUartInstance_t* uart, uint8_t* bufferIn, uint32_t bufferSz);
void cdc_itf_configure(struct CDCUartInstance_t* uart, const char* name, uint8_t itf, enum EUartType type, int32_t defaultProfileId);
void cdc_shared_poll_port_switcher(struct CDCUartInstance_t* uart);
void cdc_led_init();
void cdc_led_update();
