// USB descriptors for the RP2350 control plane: CDC (control) + vendor bulk IN
// (capture stream). Replaces pico_stdio_usb's default descriptors; stdio still
// runs over CDC 0. See tusb_config.h.

#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#include "usb_stream.h"

#define USB_VID 0x35F0
#define USB_PID 0xDB12   // docs/protocol.md: DB11 probe, DB12 control, DB13 FX2

enum {
    ITF_CDC_CTRL = 0,
    ITF_CDC_DATA,
    ITF_STREAM,          // == USB_STREAM_ITF, the capture stream
    ITF_COUNT,
};
_Static_assert(ITF_STREAM == USB_STREAM_ITF, "host tools claim this interface number");

#define EP_CDC_NOTIF   0x81
#define EP_CDC_OUT     0x02
#define EP_CDC_IN      0x82
#define EP_STREAM_OUT  0x03   // unused; the vendor class wants a pair
#define EP_STREAM_IN   USB_STREAM_EP_IN

enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_STREAM,
    STR_COUNT,
};

// bRequest the host uses to fetch the MS OS 2.0 descriptor set.
#define VENDOR_REQUEST_MICROSOFT 0x01

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,   // 2.1: required for the BOS / MS OS 2.0 descriptors
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0200,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_CDC_CTRL, STR_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_VENDOR_DESCRIPTOR(ITF_STREAM, STR_STREAM, EP_STREAM_OUT, EP_STREAM_IN, 64),
};

// --- MS OS 2.0: bind WinUSB to the stream interface, no INF, no Zadig ---

#define MS_OS_20_DESC_LEN (0x0A + 0x08 + 0x08 + 0x14 + 0x84)
#define BOS_TOTAL_LEN     (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

static const uint8_t desc_ms_os_20[] = {
    // Set header
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    // Configuration subset
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
    // Function subset: the stream interface only; CDC keeps its inbox driver
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_STREAM, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
    // Compatible ID: WINUSB
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ)
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0,
    'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0,
    0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, '5', 0, 'C', 0, '9', 0, 'D', 0, '2', 0, 'B', 0, '1', 0, 'A', 0, '-', 0,
    '7', 0, 'F', 0, '3', 0, 'E', 0, '-', 0, '4', 0, 'B', 0, '8', 0, 'A', 0, '-', 0,
    '9', 0, 'C', 0, '6', 0, '1', 0, '-', 0, '2', 0, 'D', 0, '4', 0, 'E', 0, '8', 0,
    'F', 0, '0', 0, 'A', 0, '3', 0, 'B', 0, '1', 0, '7', 0, '}', 0, 0, 0, 0, 0,
};
_Static_assert(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 length");

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

const uint8_t *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_REQUEST_MICROSOFT &&
        request->wIndex == 7) {   // MS_OS_20_DESCRIPTOR_INDEX
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20,
                                sizeof(desc_ms_os_20));
    }
    return false;   // stall anything else
}

static char s_serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const desc_strings[STR_COUNT] = {
    [STR_MANUFACTURER] = "Bitcraze AB",
    [STR_PRODUCT]      = "Bugslayer deck control",
    [STR_SERIAL]       = s_serial,   // flash unique ID; the FX2 reports the same one
    [STR_CDC]          = "Bugslayer control",
    [STR_STREAM]       = "Bugslayer capture stream",
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[32];
    uint8_t len;

    if (!s_serial[0]) pico_get_unique_board_id_string(s_serial, sizeof(s_serial));

    if (index == STR_LANGID) {
        desc[1] = 0x0409;
        len = 1;
    } else {
        if (index >= STR_COUNT || !desc_strings[index]) return NULL;
        const char *str = desc_strings[index];
        for (len = 0; len < count_of(desc) - 1 && str[len]; len++) desc[1 + len] = (uint8_t)str[len];
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}
