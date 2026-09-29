/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2021 Peter Lawrence
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
void cdc_activate_profile( int uartId, int profileId );
#include "FreeRTOS.h"
#include "task.h"

#include "hardware/clocks.h"
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <stdio.h>
#include <string.h>

#if PICO_SDK_VERSION_MAJOR >= 2
#include "bsp/board_api.h"
#else
#include "bsp/board.h"
#endif
#include "tusb.h"

#include "probe_config.h"
#include "probe.h"
#include "cdc_uart.h"
#include "get_serial.h"
#include "tusb_edpt_handler.h"
#include "DAP.h"
#include "hardware/structs/usb.h"
#include "semphr.h"

#define THREADED 1
//#define CDC_UART

#define HWUART_TASK_PRIO (tskIDLE_PRIORITY + 4) // TODO: tune
#define UART_TASK_PRIO (tskIDLE_PRIORITY + 3)
#define TUD_TASK_PRIO  (tskIDLE_PRIORITY + 2)
#define DAP_TASK_PRIO  (tskIDLE_PRIORITY + 1)

#if configMAX_PRIORITIES < 8
  #error "freeRtos not configMAX_PRIORITIES not configured"
#endif

TaskHandle_t dap_taskhandle, tud_taskhandle, mon_taskhandle, uart_taskHandle;
uint8_t cdcArg0 = 0;
uint8_t cdcArg1 = 1;
extern SemaphoreHandle_t globalMutex;

static int was_configured;

void dev_mon(void *ptr)
{
    uint32_t sof[3];
    int i = 0;
    TickType_t wake;
    wake = xTaskGetTickCount();
    do {
        /* ~5 SOF events per tick */
        xTaskDelayUntil(&wake, 100);
        if (tud_connected() && !tud_suspended()) {
            sof[i++] = usb_hw->sof_rd & USB_SOF_RD_BITS;
            i = i % 3;
        } else {
            for (i = 0; i < 3; i++)
                sof[i] = 0;
        }
        if ((sof[0] | sof[1] | sof[2]) != 0) {
            if ((sof[0] == sof[1]) && (sof[1] == sof[2])) {
                probe_info("Watchdog timeout! Resetting USBD\n");
                /* uh oh, signal disconnect (implicitly resets the controller) */
                tud_deinit(0);
                /* Make sure the port got the message */
                xTaskDelayUntil(&wake, 1);
                tud_init(0);
            }
        }
    } while (1);
}


void usb_thread(void *ptr)
{
#ifdef PROBE_USB_CONNECTED_LED
    gpio_init(PROBE_USB_CONNECTED_LED);
    gpio_set_dir(PROBE_USB_CONNECTED_LED, GPIO_OUT);
#endif
    TickType_t wake;
    wake = xTaskGetTickCount();
    do {
        cdc_led_update();
        probe_led_update();
        tud_task();
#ifdef PROBE_USB_CONNECTED_LED
        if (!gpio_get(PROBE_USB_CONNECTED_LED) && tud_ready())
        {
            gpio_put(PROBE_USB_CONNECTED_LED, 1);
        }
        else
        {
            gpio_put(PROBE_USB_CONNECTED_LED, 0);
        }
#endif
        // If suspended or disconnected, delay for 1ms (20 ticks)
        if (tud_suspended() || !tud_connected())
            xTaskDelayUntil(&wake, 20);
        // Go to sleep for up to a tick if nothing to do
        else if (!tud_task_event_ready())
            xTaskDelayUntil(&wake, 1);
    } while (1);
}


// Workaround API change in 0.13
#if (TUSB_VERSION_MAJOR == 0) && (TUSB_VERSION_MINOR <= 12)
#define tud_vendor_flush(x) ((void)0)
#endif

int main(void) 
{
    // startup flash
    gpio_init(PROBE_USB_CONNECTED_LED);
    gpio_set_dir(PROBE_USB_CONNECTED_LED, GPIO_OUT);    
    gpio_put(PROBE_USB_CONNECTED_LED,1);

    probe_led_init();
    cdc_led_init();

    set_sys_clock_khz(252000, true);
    sleep_ms(100);

    globalMutex = xSemaphoreCreateMutex();

    memset(uartDevices, 0, sizeof(uartDevices));

    cdc_itf_configure(&uartDevices[0], "ITF0", 0, EUartType_Hardware, 0);
    cdc_itf_configure(&uartDevices[1], "ITF1", 1, EUartType_Hardware, 1);
    
    // Declare pins in binary information
    bi_decl_config();

    board_init();
    usb_serial_init();

    tusb_init();
    
    for(int i=0;i<MAX_DAP_PROBES;i++)
      DAP_Setup(i);

    probe_info("Welcome to debugprobe!\n");

    if (THREADED) {
        xTaskCreate(usb_thread, "TUD", configMINIMAL_STACK_SIZE, NULL, TUD_TASK_PRIO, &tud_taskhandle);
#if PICO_RP2040
        xTaskCreate(dev_mon, "WDOG", configMINIMAL_STACK_SIZE, NULL, TUD_TASK_PRIO, &mon_taskhandle);
#endif        
        xTaskCreate(cdc_uart_core2, "UART", configMINIMAL_STACK_SIZE, NULL, HWUART_TASK_PRIO, &uart_taskHandle);

        vTaskStartScheduler();
    }

    return 0;
}


#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
extern uint8_t const desc_ms_os_20[];

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request)
{
  // nothing to with DATA & ACK stage
  if (stage != CONTROL_STAGE_SETUP) return true;

  switch (request->bmRequestType_bit.type)
  {
    case TUSB_REQ_TYPE_VENDOR:
      switch (request->bRequest)
      {
        case 1:
          if ( request->wIndex == 7 )
          {
            // Get Microsoft OS 2.0 compatible descriptor
            uint16_t total_len;
            memcpy(&total_len, desc_ms_os_20+8, 2);

            return tud_control_xfer(rhport, request, (void*) desc_ms_os_20, total_len);
          }
          else
          {
            return false;
          }

        default: break;
      }
    break;
    default: break;
  }

  // stall unknown request
  return false;
}
#endif

void tud_suspend_cb(bool remote_wakeup_en)
{
  probe_info("Suspended\n");
  /* Were we actually configured? If not, threads don't exist */
  if (was_configured) {
    for(int i=0;i<MAX_UART_DEVICES;i++)
    {
      if(uartDevices[i].isConfigured)
  	    vTaskSuspend(uartDevices[i].uart_taskHandle);
    }  
	  vTaskSuspend(dap_taskhandle);
  }
  /* slow down clk_sys for power saving ? */
}

void tud_resume_cb(void)
{
  probe_info("Resumed\n");
  if (was_configured) {
    for(int i=0;i<MAX_UART_DEVICES;i++)
    {
      if(uartDevices[i].isConfigured)
  	    vTaskResume(uartDevices[i].uart_taskHandle);
    } 
	  vTaskResume(dap_taskhandle);
  }
}

void tud_unmount_cb(void)
{
  probe_info("Disconnected\n");
  for(int i=0;i<MAX_UART_DEVICES;i++)
  {
    if(uartDevices[i].isConfigured)      
      vTaskSuspend(uartDevices[i].uart_taskHandle);
  }
  vTaskSuspend(dap_taskhandle);
  for(int i=0;i<MAX_UART_DEVICES;i++)
  {
    if(uartDevices[i].isConfigured)      
      vTaskDelete(uartDevices[i].uart_taskHandle);
  }
  vTaskDelete(dap_taskhandle);
  was_configured = 0;
}


void tud_mount_cb(void)
{  
  if (!was_configured) {
    /* UART needs to preempt USB as if we don't, characters get lost */

    for(int i=0;i<MAX_UART_DEVICES;i++)    
    {
      if(uartDevices[i].isConfigured)
      {
        switch (uartDevices[i].type)
        {
        case EUartType_Hardware:
          xTaskCreate(cdc_uart_thread, uartDevices[i].name, configMINIMAL_STACK_SIZE, &uartDevices[i], UART_TASK_PRIO, &uartDevices[i].uart_taskHandle);
          break;                          
        default:
          break;
        }        
      }
    }   

    /* Lowest priority thread is debug - need to shuffle buffers before we can toggle swd... */    
    for(int i=0;i<MAX_DAP_PROBES;i++)
      xTaskCreate(dap_thread, "DAP.x", configMINIMAL_STACK_SIZE, getEndpointStateByIndex(i), DAP_TASK_PRIO, &getEndpointStateByIndex(i)->dap_taskhandle);   

    was_configured = 1;
  }
}

void vApplicationTickHook (void)
{
};

void vApplicationStackOverflowHook(TaskHandle_t Task, char *pcTaskName)
{
  panic("stack overflow (not the helpful kind) for %s\n", *pcTaskName);
}

void vApplicationMallocFailedHook(void)
{
  panic("Malloc Failed\n");
};
