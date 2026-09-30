/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
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

#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "tusb.h"
#include <stdlib.h>

#include "cdc_uart.h"
#include "probe_config.h"

SemaphoreHandle_t globalMutex;
extern TaskHandle_t uart_taskHandle;


static void lock_hw_uart()
{
  xSemaphoreTake( globalMutex, portMAX_DELAY );
}


static void unlock_hw_uart()
{
  xSemaphoreGive( globalMutex );
}


// Drains every bound UART's RX FIFO into its ring. At 2 Mbaud the 32-byte FIFO
// fills in 160 us, so this runs every tick (50 us) at the highest priority.
void cdc_uart_core2( void* ptr)
{
    while (true)
    {
      if( xSemaphoreTake( globalMutex, pdMS_TO_TICKS(1) ) == pdTRUE )
      {
        for(int i=0;i<MAX_UART_DEVICES;i++)
        {
          struct CDCUartInstance_t* uart = &uartDevices[i];
          if(!uart->isUartInitialised)
            continue;
          for(int n=0; n<32 && uart_is_readable(uart->uart); n++)
          {
            uint8_t c = (uint8_t)uart_get_hw(uart->uart)->dr;
            uint32_t next = (uart->writingBuffer + 1) % CDC_BUFFER_SZ;
            if(next == uart->readingBuffer)
            {
              uart->bufferOverflow++;
              continue;
            }
            uart->buffer[ uart->writingBuffer ] = c;
            __dmb();  // the byte before the index: the reader may run on the other core
            uart->writingBuffer = next;
          }
        }
        xSemaphoreGive( globalMutex );
      }

      TickType_t wake;
      wake = xTaskGetTickCount();      
      xTaskDelayUntil(&wake, 1);   
    }
}


// Apply the host's line coding to a bound UART. TinyUSB keeps the last one the
// host set, so a UART bound after the port was opened still gets it. Call with
// the UART lock held.
static void apply_line_coding(struct CDCUartInstance_t* uart)
{
  cdc_line_coding_t line_coding;
  uart_parity_t parity;
  uint data_bits, stop_bits;

  tud_cdc_n_get_line_coding(uart->itf, &line_coding);

  /* Set the tick thread interval to the amount of time it takes to
   * fill up half a FIFO. Millis is too coarse for integer divide.
   */
  uint32_t micros = (1000 * 1000 * 16 * 10) / MAX(line_coding.bit_rate, 1);
  uart->interval = MAX(1, micros / ((1000 * 1000) / configTICK_RATE_HZ));
  uart->baud = line_coding.bit_rate;
  probe_info("New baud rate %ld micros %ld interval %lu\n",
                  line_coding.bit_rate, micros, uart->interval);

  switch (line_coding.parity) {
  case CDC_LINE_CODING_PARITY_ODD:
    parity = UART_PARITY_ODD;
    break;
  case CDC_LINE_CODING_PARITY_EVEN:
    parity = UART_PARITY_EVEN;
    break;
  default:
    probe_info("invalid parity setting %u\n", line_coding.parity);
    /* fallthrough */
  case CDC_LINE_CODING_PARITY_NONE:
    parity = UART_PARITY_NONE;
    break;
  }

  switch (line_coding.data_bits) {
  case 5:
  case 6:
  case 7:
  case 8:
    data_bits = line_coding.data_bits;
    break;
  default:
    probe_info("invalid data bits setting: %u\n", line_coding.data_bits);
    data_bits = 8;
    break;
  }

  /* The PL011 only supports 1 or 2 stop bits. 1.5 stop bits is translated to 2,
   * which is safer than the alternative. */
  switch (line_coding.stop_bits) {
  case CDC_LINE_CONDING_STOP_BITS_1_5:
  case CDC_LINE_CONDING_STOP_BITS_2:
    stop_bits = 2;
  break;
  default:
    probe_info("invalid stop bits setting: %u\n", line_coding.stop_bits);
    /* fallthrough */
  case CDC_LINE_CONDING_STOP_BITS_1:
    stop_bits = 1;
  break;
  }

  uart_set_baudrate(uart->uart, line_coding.bit_rate);
  uart_set_format(uart->uart, data_bits, stop_bits, parity);
}


void cdc_uart_init(struct CDCUartInstance_t* uart, const struct UartProfile_t* profile) 
{
  // A profile with no RX pin leaves the CDC port unbound
  if(!profile || profile->rxPin == -1)
    return;

  lock_hw_uart();

  if(uart->isUartInitialised)
  {
    unlock_hw_uart();
    return;
  }

  switch (profile->hwUartId)
  {   
  case 0:
    uart->uart = uart0;
    break;
  case 1:
    uart->uart = uart1;    
    break;
  }

  uart->currentProfile = profile;
  uart->nextProfile = 0;
  uart->baud = profile->baud;  
  uart->rxPin = profile->rxPin;
  uart->txPin = profile->txPin;

  memset(uart->matchKeyStates, 0, sizeof(uart->matchKeyStates)); 
  gpio_set_function(uart->rxPin, GPIO_FUNC_UART);
  if(uart->txPin != -1)
    gpio_set_function(uart->txPin, GPIO_FUNC_UART);
  gpio_set_pulls(uart->rxPin, 1, 0);   
  uart_init(uart->uart, uart->baud);
  apply_line_coding(uart);

#ifdef PROBE_UART_HWFC
    // HWFC implies that hardware flow control is implemented and the
    // UART operates in "full-duplex" mode (See USB CDC PSTN120 6.3.12).
    // Default to pulling in the active direction, so an unconnected CTS
    // behaves the same as if CTS were not enabled. 
    gpio_set_pulls(uart->ctsPin, 0, 1);
    gpio_set_function(uart->rtsPin, GPIO_FUNC_UART);
    gpio_set_function(uart->ctsPin, GPIO_FUNC_UART);
    uart_set_hw_flow(PROBE_UART_INTERFACE, true, true);
#else
#ifdef PROBE_UART_RTS
    gpio_init(uart->rtsPin);
    gpio_set_dir(uart->rtsPin, GPIO_OUT);
    gpio_put(uart->rtsPin, 1);
#endif
#endif

#ifdef PROBE_UART_DTR
    gpio_init(PROBE_UART_DTR);
    gpio_set_dir(PROBE_UART_DTR, GPIO_OUT);
    gpio_put(PROBE_UART_DTR, 1);
#endif

  uart->isUartInitialised = true;

  unlock_hw_uart();
}


void cdc_uart_deinit(struct CDCUartInstance_t* uart)
{      
  lock_hw_uart();

  if(!uart->isUartInitialised)
  {
    unlock_hw_uart();
    return;
  }

  if(uart->uart)
    uart_deinit(uart->uart);
  
  if(uart->rxPin != -1)
    gpio_set_function(uart->rxPin, GPIO_FUNC_NULL);
  if(uart->txPin != -1)
    gpio_set_function(uart->txPin, GPIO_FUNC_NULL);     

  uart->currentProfile = 0;  
  uart->baud = -1;
  uart->rxPin = -1;
  uart->txPin = -1;    
  uart->uart = 0;
      
  uart->isUartInitialised = false;

  unlock_hw_uart();
}


// Custom vendor trigger
void cdc_activate_profile( int uartId, int profileId )
{
  if(uartId >= MAX_UART_DEVICES || profileId >= MAX_UART_PROFILES)
    return;

  struct CDCUartInstance_t* uartInst = &uartDevices[uartId];
  uartInst->nextProfile = &uartProfiles[profileId];
}


void cdc_handle_profile_switch(struct CDCUartInstance_t* uart)
{
  // Selecting the bound profile again changes nothing
  if(uart->nextProfile && uart->nextProfile == uart->currentProfile && uart->isUartInitialised)
    uart->nextProfile = 0;

  if(uart->nextProfile)
  {              
    // Release matching hw
    for(int i=0;i<MAX_UART_DEVICES;i++)
    {
      // Other device
      if(&uartDevices[i] != uart)
      {
        // Has profile
        if(uartDevices[i].currentProfile)
        {
          // Same hw
          if(uartDevices[i].currentProfile->hwUartId == uart->nextProfile->hwUartId)
          {
            // Release it
            cdc_uart_deinit(&uartDevices[i]);
          }
        }        
      }
    }

    

    // de-init current
    cdc_uart_deinit(uart);

    // bind uart to profile
    cdc_uart_init(uart, uart->nextProfile);
  }

  uart->nextProfile = 0;
}


bool cdc_uart_task(struct CDCUartInstance_t* uart)
{
    // Profile switch
    cdc_handle_profile_switch(uart);

    // Ensure uart
    if(!uart->isUartInitialised)
      return false;

    bool keep_alive = false;

    if (tud_cdc_n_connected(uart->itf)) 
    {
        if(!uart->was_connected)
        {
          // What arrived while nobody listened is stale
          uart->readingBuffer = uart->writingBuffer;
          uart->was_connected = 1;
        }

        uint32_t end = uart->writingBuffer;
        __dmb();
        if(uart->readingBuffer != end)
        {
          while(uart->readingBuffer != end)
          {
            // Contiguous part of the ring; what TinyUSB cannot take now
            // stays in the ring for the next poll.
            uint32_t r = uart->readingBuffer;
            uint32_t n = (end > r ? end : CDC_BUFFER_SZ) - r;
            uint32_t sent = tud_cdc_n_write(uart->itf, &uart->buffer[r], n);
            if(sent == 0)
              break;
            uart->readingBuffer = (r + sent) % CDC_BUFFER_SZ;
            uart->lastActivityTimeRx = time_us_32();
          }

          tud_cdc_n_write_flush(uart->itf);
        }

      /* Reading from a firehose and writing to a FIFO. */
      size_t watermark = MIN(tud_cdc_n_available(uart->itf), sizeof(uart->tx_buf));
      if (watermark > 0) {
        size_t tx_len;

        /* Batch up to half a FIFO of data - don't clog up on RX */
        watermark = MIN(watermark, 16);
        tx_len = tud_cdc_n_read(uart->itf, uart->tx_buf, watermark);
   
        uart_write_blocking(uart->uart, uart->tx_buf, tx_len);

        uart->lastActivityTimeTx = time_us_32();
      } 

      /* Pending break handling */
      if (uart->timed_break) 
      {
        if (((int)uart->break_expiry - (int)xTaskGetTickCount()) < 0) {
          uart->timed_break = false;          
          uart_set_break(uart->uart, false);
        } 
        else 
        {
          keep_alive = true;
        }
      }
    } 
    else if (uart->was_connected) 
    {
      tud_cdc_n_write_clear(uart->itf);

      if(uart->uart)
        uart_set_break(uart->uart, false);

      uart->timed_break = false;
      uart->was_connected = 0;
      uart->cdc_tx_oe = 0;
    }

    return keep_alive;
}


void cdc_uart_thread(void *ptr)
{
  struct CDCUartInstance_t* uart = (CDCUartInstance_t*)ptr;
  if( !uart->isConfigured )
    return;  
  BaseType_t delayed;
  uart->last_wake = xTaskGetTickCount();
  bool keep_alive;
  /* Threaded with a polling interval that scales according to linerate */
  while (1) {
    keep_alive = cdc_uart_task(uart);
    if (!keep_alive) {
      delayed = xTaskDelayUntil(&uart->last_wake, uart->interval);
        if (delayed == pdFALSE)
          uart->last_wake = xTaskGetTickCount();
    }
  }
}


void cdc_uart_line_coding_cb(struct CDCUartInstance_t* uart, uint8_t itf, cdc_line_coding_t const* line_coding)
{  
  lock_hw_uart();
  if(uart->isUartInitialised)
    apply_line_coding(uart);
  unlock_hw_uart();
}


void cdc_uart_line_state_cb(struct CDCUartInstance_t* uart, uint8_t itf, bool dtr, bool rts)
{  
#ifdef PROBE_UART_RTS
  gpio_put(uart->rtsPin, !rts);
#endif
#ifdef PROBE_UART_DTR
  gpio_put(PROBE_UART_DTR, !dtr);
#endif

  /* CDC drivers use linestate as a bodge to activate/deactivate the interface.
   * Resume our UART polling on activate, stop on deactivate */
  if (!dtr) {
    vTaskSuspend(uart->uart_taskHandle);
#ifdef PROBE_UART_RX_LED
    gpio_put(PROBE_UART_RX_LED, 0);
    rx_led_debounce = 0;
#endif
#ifdef PROBE_UART_TX_LED
    gpio_put(PROBE_UART_TX_LED, 0);
    tx_led_debounce = 0;
#endif
  } else
    vTaskResume(uart->uart_taskHandle);
}

