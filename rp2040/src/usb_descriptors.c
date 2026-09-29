/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2021 Peter Lawrence
 * Copyright (c) 2022 Raspberry Pi Ltd
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

#include "tusb.h"
#include "get_serial.h"
#include "probe_config.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
    .bcdUSB             = 0x0210, // USB Specification version 2.1 for BOS
#else
    .bcdUSB             = 0x0110,
#endif
    .bDeviceClass       = 0x00, // Each interface specifies its own
    .bDeviceSubClass    = 0x00, // Each interface specifies its own
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x2E8A, // Pi+1
    .idProduct          = 0x000c, // CMSIS-DAP Debug Probe
    .bcdDevice          = 0x0222, // Version 02.22
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum
{
  ITF_NUM_PROBE, // Old versions of Keil MDK only look at interface 0
  ITF1_NUM_PROBE, 
  ITF2_NUM_PROBE, 
  ITF3_NUM_PROBE, 
  ITF_NUM_CDC_COM,
  ITF_NUM_CDC_DATA,
  ITF_NUM_CDC1_COM,
  ITF_NUM_CDC1_DATA, 
  ITF_NUM_TOTAL
};

#define CDC_NOTIFICATION_EP_NUM 0x81
#define CDC_DATA_OUT_EP_NUM 0x02
#define CDC_DATA_IN_EP_NUM 0x83
#define DAP_OUT_EP_NUM 0x04
#define DAP_IN_EP_NUM 0x85
#define DAP1_OUT_EP_NUM 0x06
#define DAP1_IN_EP_NUM 0x87
#define DAP2_OUT_EP_NUM 0x08
#define DAP2_IN_EP_NUM 0x89

#define DAP3_OUT_EP_NUM 0xA
#define DAP3_IN_EP_NUM 0x8B

#define CDC1_NOTIFICATION_EP_NUM   0x8d
#define CDC1_DATA_OUT_EP_NUM       0x0e
#define CDC1_DATA_IN_EP_NUM        0x8e

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN)

uint8_t desc_configuration[] =
{
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
  // Bulk (named interface)
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_PROBE, 5, DAP_OUT_EP_NUM, DAP_IN_EP_NUM, 64),
  TUD_VENDOR_DESCRIPTOR(ITF1_NUM_PROBE, 5, DAP1_OUT_EP_NUM, DAP1_IN_EP_NUM, 64),
  TUD_VENDOR_DESCRIPTOR(ITF2_NUM_PROBE, 5, DAP2_OUT_EP_NUM, DAP2_IN_EP_NUM, 64),
  TUD_VENDOR_DESCRIPTOR(ITF3_NUM_PROBE, 5, DAP3_OUT_EP_NUM, DAP3_IN_EP_NUM, 64),
  // Interface 1 + 2
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_COM, 6, CDC_NOTIFICATION_EP_NUM, 64, CDC_DATA_OUT_EP_NUM, CDC_DATA_IN_EP_NUM, 64),
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC1_COM, 7, CDC1_NOTIFICATION_EP_NUM, 64, CDC1_DATA_OUT_EP_NUM, CDC1_DATA_IN_EP_NUM, 64),       
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index; // for multiple configurations
  /* Hack in CAP_BREAK support */
  desc_configuration[CONFIG_TOTAL_LEN - TUD_CDC_DESC_LEN + 8 + 9 + 5 + 5 + 4 - 1] = 0x6;
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
char const* string_desc_arr [] =
{
  (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
  "Multiprobe", // 1: Manufacturer
  PROBE_PRODUCT_STRING, // 2: Product
  usb_serial,     // 3: Serial, uses flash unique ID
  "CMSIS-DAP v1 Interface", // 4: Interface descriptor for HID transport
  "CMSIS-DAP v2 Interface", // 5: Interface descriptor for Bulk transport
  "CDC-ACM UART Interface", // 6: Interface descriptor for CDC
  "Custom CDC-ACM UART Interface", // 6: Interface descriptor for CDC
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;

  if ( index == 0)
  {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  }else
  {
    // Convert ASCII string into UTF-16

    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];

    // Cap at max char
    chr_count = strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    for(uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

  return _desc_str;
}

/* [incoherent gibbering to make Windows happy] */

//--------------------------------------------------------------------+
// BOS Descriptor
//--------------------------------------------------------------------+

/* Microsoft OS 2.0 registry property descriptor
Per MS requirements https://msdn.microsoft.com/en-us/library/windows/hardware/hh450799(v=vs.85).aspx
device should create DeviceInterfaceGUIDs. It can be done by driver and
in case of real PnP solution device should expose MS "Microsoft OS 2.0
registry property descriptor". Such descriptor can insert any record
into Windows registry per device/configuration/interface. In our case it
will insert "DeviceInterfaceGUIDs" multistring property.


https://developers.google.com/web/fundamentals/native-hardware/build-for-webusb/
(Section Microsoft OS compatibility descriptors)
*/
#define MS_OS_20_DESC_LEN  658

#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

uint8_t const desc_bos[] =
{
  // total length, number of device caps
  TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

  // Microsoft OS 2.0 descriptor
  TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1)
};

uint8_t const desc_ms_os_20[] =
{
    // Set header: length, type, windows version, total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header: length, type, configuration index, reserved, configuration total length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function Subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_PROBE, 0, U16_TO_U8S_LE(0x08 + 0x14 + 0x84),

    // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible
    
    // MS OS 2.0 Registry property descriptor: length, type
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    //bPropertyData: {70394F16-EDAF-47D5-92C8-7CB511020303}.
    '{', 0x00, '7', 0x00, '0', 0x00, '3', 0x00, '9', 0x00, '4', 0x00, 'F', 0x00, '1', 0x00, '6', 0x00, '-', 0x00,
    'E', 0x00, 'D', 0x00, 'A', 0x00, 'F', 0x00, '-', 0x00, '4', 0x00, '7', 0x00, 'D', 0x00, '5', 0x00, '-', 0x00,
    '9', 0x00, '2', 0x00, 'C', 0x00, '8', 0x00, '-', 0x00, '7', 0x00, 'C', 0x00, 'B', 0x00, '5', 0x00, '1', 0x00,
    '1', 0x00, '0', 0x00, '2', 0x00, '0', 0x00, '3', 0x00, '0', 0x00, '3', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
  
    // Function Subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF1_NUM_PROBE, 0, U16_TO_U8S_LE(0x08 + 0x14 + 0x84),

    // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible
    
    // MS OS 2.0 Registry property descriptor: length, type
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    //bPropertyData: {70394F16-EDAF-47D5-92C8-7CB511020304}.
    '{', 0x00, '7', 0x00, '0', 0x00, '3', 0x00, '9', 0x00, '4', 0x00, 'F', 0x00, '1', 0x00, '6', 0x00, '-', 0x00,
    'E', 0x00, 'D', 0x00, 'A', 0x00, 'F', 0x00, '-', 0x00, '4', 0x00, '7', 0x00, 'D', 0x00, '5', 0x00, '-', 0x00,
    '9', 0x00, '2', 0x00, 'C', 0x00, '8', 0x00, '-', 0x00, '7', 0x00, 'C', 0x00, 'B', 0x00, '5', 0x00, '1', 0x00,
    '1', 0x00, '0', 0x00, '2', 0x00, '0', 0x00, '3', 0x00, '0', 0x00, '4', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,

    // Function Subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF2_NUM_PROBE, 0, U16_TO_U8S_LE(0x08 + 0x14 + 0x84),

    // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible
    
    // MS OS 2.0 Registry property descriptor: length, type
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    //bPropertyData: {70394F16-EDAF-47D5-92C8-7CB511020305}.
    '{', 0x00, '7', 0x00, '0', 0x00, '3', 0x00, '9', 0x00, '4', 0x00, 'F', 0x00, '1', 0x00, '6', 0x00, '-', 0x00,
    'E', 0x00, 'D', 0x00, 'A', 0x00, 'F', 0x00, '-', 0x00, '4', 0x00, '7', 0x00, 'D', 0x00, '5', 0x00, '-', 0x00,
    '9', 0x00, '2', 0x00, 'C', 0x00, '8', 0x00, '-', 0x00, '7', 0x00, 'C', 0x00, 'B', 0x00, '5', 0x00, '1', 0x00,
    '1', 0x00, '0', 0x00, '2', 0x00, '0', 0x00, '3', 0x00, '0', 0x00, '5', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,    
    
    // Function Subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF3_NUM_PROBE, 0, U16_TO_U8S_LE(0x08 + 0x14 + 0x84),

    // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible
    
    // MS OS 2.0 Registry property descriptor: length, type
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    //bPropertyData: {70394F16-EDAF-47D5-92C8-7CB511020306}.
    '{', 0x00, '7', 0x00, '0', 0x00, '3', 0x00, '9', 0x00, '4', 0x00, 'F', 0x00, '1', 0x00, '6', 0x00, '-', 0x00,
    'E', 0x00, 'D', 0x00, 'A', 0x00, 'F', 0x00, '-', 0x00, '4', 0x00, '7', 0x00, 'D', 0x00, '5', 0x00, '-', 0x00,
    '9', 0x00, '2', 0x00, 'C', 0x00, '8', 0x00, '-', 0x00, '7', 0x00, 'C', 0x00, 'B', 0x00, '5', 0x00, '1', 0x00,
    '1', 0x00, '0', 0x00, '2', 0x00, '0', 0x00, '3', 0x00, '0', 0x00, '6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,    
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

uint8_t const * tud_descriptor_bos_cb(void)
{
  return desc_bos;
}