// Bugslayer deck — CBM9002A (FX2LP) slave FIFO bridge.
//
// Sync slave FIFO, IN-only, AUTOIN, with IFCLK driven externally by the
// RP2350. After boot the 8051 is out of the data path entirely: it answers
// control requests on EP0 and does nothing else.
//
// Design notes: Obsidian Projects/Bugslayer/Deck/FX2 firmware.md
// Reference:    Glasgow firmware/fx2/ (0-BSD), libfx2 (0-BSD)

#include <fx2delay.h>
#include <fx2lib.h>
#include <fx2regs.h>
#include <fx2usb.h>
#include <usbmicrosoft.h>

// TODO: allocate real IDs. 1209:0001 is pid.codes' test PID and must not ship.
// The Crazyflie itself uses Nordic's VID (1915), which may be the precedent.
#define USB_VID 0x1209
#define USB_PID 0x0001

// Windows caches the MS OS descriptor answer per VID/PID/bcdDevice. Bump the
// low byte on every descriptor change during development or you will chase a
// stale cache for an afternoon.
#define USB_BCD_DEVICE 0x0001

enum {
    USB_DESC_STRING_MS_INDEX   = 0xEE,
    USB_REQ_GET_MS_DESCRIPTOR  = 0xC0,
};

usb_desc_device_c usb_device = {
    .bLength              = sizeof(struct usb_desc_device),
    .bDescriptorType      = USB_DESC_DEVICE,
    .bcdUSB               = 0x0200,
    .bDeviceClass         = USB_DEV_CLASS_VENDOR,
    .bDeviceSubClass      = USB_DEV_SUBCLASS_VENDOR,
    .bDeviceProtocol      = USB_DEV_PROTOCOL_VENDOR,
    .bMaxPacketSize0      = 64,
    .idVendor             = USB_VID,
    .idProduct            = USB_PID,
    .bcdDevice            = USB_BCD_DEVICE,
    .iManufacturer        = 1,
    .iProduct             = 2,
    .iSerialNumber        = 3,
    .bNumConfigurations   = 1,
};

usb_desc_device_qualifier_c usb_device_qualifier = {
    .bLength              = sizeof(struct usb_desc_device_qualifier),
    .bDescriptorType      = USB_DESC_DEVICE_QUALIFIER,
    .bcdUSB               = 0x0200,
    .bDeviceClass         = USB_DEV_CLASS_PER_INTERFACE,
    .bDeviceSubClass      = USB_DEV_SUBCLASS_PER_INTERFACE,
    .bDeviceProtocol      = USB_DEV_PROTOCOL_PER_INTERFACE,
    .bMaxPacketSize0      = 64,
    .bNumConfigurations   = 0,
};

usb_desc_interface_c usb_interface = {
    .bLength              = sizeof(struct usb_desc_interface),
    .bDescriptorType      = USB_DESC_INTERFACE,
    .bInterfaceNumber     = 0,
    .bAlternateSetting    = 0,
    .bNumEndpoints        = 1,
    .bInterfaceClass      = USB_IFACE_CLASS_VENDOR,
    .bInterfaceSubClass   = USB_IFACE_SUBCLASS_VENDOR,
    .bInterfaceProtocol   = USB_IFACE_PROTOCOL_VENDOR,
    .iInterface           = 0,
};

usb_desc_endpoint_c usb_endpoint_ep6_in = {
    .bLength              = sizeof(struct usb_desc_endpoint),
    .bDescriptorType      = USB_DESC_ENDPOINT,
    .bEndpointAddress     = 6|USB_DIR_IN,
    .bmAttributes         = USB_XFER_BULK,
    .wMaxPacketSize       = 512,
    .bInterval            = 0,
};

usb_configuration_c usb_config = {
    {
        .bLength              = sizeof(struct usb_desc_configuration),
        .bDescriptorType      = USB_DESC_CONFIGURATION,
        .bNumInterfaces       = 1,
        .bConfigurationValue  = 1,
        .iConfiguration       = 0,
        .bmAttributes         = USB_ATTR_RESERVED_1,
        .bMaxPower            = 50,   // 100 mA
    },
    {
        { .interface = &usb_interface },
        { .endpoint  = &usb_endpoint_ep6_in },
        { 0 }
    }
};

usb_configuration_set_c usb_configs[] = {
    &usb_config,
};

// String 3 is a placeholder the RP2350 patches when it serves the boot image,
// so this device and the RP2350's own USB device report the same serial and
// the host can pair them. The marker is what the patch tool searches for; keep
// the length fixed. See ../../docs/protocol.md.
usb_ascii_string_c usb_strings[] = {
    [0] = "Bitcraze AB",
    [1] = "Bugslayer deck capture",
    [2] = "BSLYSERIAL000000",
};

usb_descriptor_set_c usb_descriptor_set = {
    .device            = &usb_device,
    .device_qualifier  = &usb_device_qualifier,
    .config_count      = ARRAYSIZE(usb_configs),
    .configs           = usb_configs,
    .string_count      = ARRAYSIZE(usb_strings),
    .strings           = usb_strings,
};

// MS OS 1.0 descriptors. This is what makes Windows 8.1+ bind WinUSB with no
// INF and no Zadig -- the entire reason the FX2 was chosen over the FT232H.
// It only works when we enumerate with our own VID/PID, i.e. when the boot
// image comes from the (emulated) EEPROM rather than a host RAM load.
usb_desc_microsoft_v10_c usb_microsoft = {
    .bLength          = sizeof(struct usb_desc_microsoft_v10),
    .bDescriptorType  = USB_DESC_STRING,
    .qwSignature      = USB_DESC_MICROSOFT_V10_SIGNATURE,
    .bMS_VendorCode   = USB_REQ_GET_MS_DESCRIPTOR,
};

usb_desc_ms_ext_compat_id_c usb_ms_ext_compat_id = {
    .dwLength         = sizeof(struct usb_desc_ms_ext_compat_id) +
                        sizeof(struct usb_desc_ms_compat_function),
    .bcdVersion       = 0x0100,
    .wIndex           = USB_DESC_MS_EXTENDED_COMPAT_ID,
    .bCount           = 1,
    .functions        = {
        {
            .bFirstInterfaceNumber = 0,
            .bReserved1            = 1,
            .compatibleID          = "WINUSB",
        },
    }
};

void handle_usb_get_descriptor(enum usb_descriptor type, uint8_t index) {
    if (type == USB_DESC_STRING && index == USB_DESC_STRING_MS_INDEX) {
        xmemcpy(scratch, (__xdata void *)&usb_microsoft, usb_microsoft.bLength);
        SETUP_EP0_IN_DESC(scratch);
    } else {
        usb_serve_descriptor(&usb_descriptor_set, type, index);
    }
}

void handle_usb_setup(__xdata struct usb_req_setup *req) {
    if (req->bmRequestType == (USB_RECIP_DEVICE|USB_TYPE_VENDOR|USB_DIR_IN) &&
        req->bRequest == USB_REQ_GET_MS_DESCRIPTOR &&
        req->wIndex   == USB_DESC_MS_EXTENDED_COMPAT_ID) {
        xmemcpy(scratch, (__xdata void *)&usb_ms_ext_compat_id,
                usb_ms_ext_compat_id.dwLength);
        SETUP_EP0_IN_DESC(scratch);
        return;
    }
    STALL_EP0();
}

// Everything below runs once. After this the 8051 never touches a data byte.
static void fifo_init(void) {
    // External IFCLK (IFCLKSRC=0), synchronous (ASYNC=0), slave FIFO (IFCFG=11).
    // IFCLK must already be toggling on the pin when this executes: with
    // IFCLKSRC=0 the FIFO register block is clocked from it, and the RP2350
    // starts the clock before releasing FX_RESET#.
    SYNCDELAY; IFCONFIG = _IFCFG1|_IFCFG0;

    // EP6 is the only endpoint. Clearing VALID on the others gives it the
    // whole 4 KB of endpoint RAM.
    SYNCDELAY; EP2CFG = 0;
    SYNCDELAY; EP4CFG = 0;
    SYNCDELAY; EP8CFG = 0;
    SYNCDELAY; EP6CFG = _VALID|_DIR|_TYPE1;   // 0xE0: bulk IN, 512 B, quad-buffered

    // WORDWIDE defaults SET. Leaving it would steal PORTD as the high half of
    // the bus and quietly break the 8-bit assumption everywhere else.
    SYNCDELAY; EP2FIFOCFG = 0;
    SYNCDELAY; EP4FIFOCFG = 0;
    SYNCDELAY; EP8FIFOCFG = 0;
    SYNCDELAY; EP6FIFOCFG = _AUTOIN|_ZEROLENIN;   // 0x0C, 8-bit, auto-commit

    // Commit every 512 bytes, so one USB packet is exactly one protocol block.
    SYNCDELAY; EP6AUTOINLENH = 0x02;
    SYNCDELAY; EP6AUTOINLENL = 0x00;

    SYNCDELAY; FIFOPINPOLAR = 0;      // SLWR#, PKTEND#, FLAGx all active low
    SYNCDELAY; PINFLAGSAB   = 0xE0;   // FLAGB = EP6 FF (nibble 0b1110); FLAGA unused

    SYNCDELAY; FIFORESET = _NAKALL;
    SYNCDELAY; FIFORESET = _NAKALL|6;
    SYNCDELAY; FIFORESET = 0;
}

int main(void) {
    // Enhanced packet handling. Required for sane FIFO behaviour on FX2LP.
    REVCTL = _ENH_PKT|_DYN_OUT;

    // 48 MHz core. CLKOUT is deliberately not driven: the RP2350 owns IFCLK.
    CPUCS = _CLKSPD1;

    fifo_init();

    usb_init(/*disconnect=*/true);
    while (1);
}
