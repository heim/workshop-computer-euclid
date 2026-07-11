// USB-descriptorer for Euclid4 — én enkelt CDC (USB-serial) enhet.
// Basert på TinyUSB sitt cdc-eksempel (MIT), forenklet til kun CDC.

#include "tusb.h"
#include "pico/unique_id.h"

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
#define USB_VID 0xCafe
#define USB_PID 0x4001   // vilkårlig; WebSerial filtrerer ikke på VID/PID
#define USB_BCD 0x0200

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    // Interface Association Descriptor (IAD) kreves for CDC:
    // klasse = Misc (0xEF), subklasse = Common (2), protokoll = IAD (1)
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// Kalt ved GET DEVICE DESCRIPTOR
uint8_t const *tud_descriptor_device_cb(void)
{
	return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
enum
{
	ITF_NUM_CDC = 0,
	ITF_NUM_CDC_DATA,
	ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82

uint8_t const desc_fs_configuration[] = {
	// Config: antall interfaces, config-nummer, streng-indeks, total lengde,
	// attributt, strøm (mA)
	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

	// CDC: interface-nummer, streng-indeks, EP notif addr/size, EP out, EP in, EP size
	TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

// Kalt ved GET CONFIGURATION DESCRIPTOR. Én config -> ignorer index.
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
enum
{
	STRID_LANGID = 0,
	STRID_MANUFACTURER,
	STRID_PRODUCT,
	STRID_SERIAL,
	STRID_CDC_INTERFACE,
};

// Serienummer bygges fra flash-unik-ID ved oppstart
static char serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static char const *string_desc_arr[] = {
	(const char[]){0x09, 0x04},   // 0: språk = engelsk (0x0409)
	"Music Thing Modular",        // 1: produsent
	"Workshop Computer — Euclid4",// 2: produkt
	serial_str,                   // 3: serienummer (fylles inn under)
	"Euclid4 Serial",             // 4: CDC-interface
};

static uint16_t _desc_str[32];

// Kalt ved GET STRING DESCRIPTOR. Returnerer peker til descriptor i _desc_str.
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;

	// Bygg serienummeret én gang
	if (serial_str[0] == 0)
		pico_get_unique_board_id_string(serial_str, sizeof(serial_str));

	size_t chr_count;

	if (index == STRID_LANGID)
	{
		memcpy(&_desc_str[1], string_desc_arr[0], 2);
		chr_count = 1;
	}
	else
	{
		if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
			return NULL;

		const char *str = string_desc_arr[index];
		chr_count = strlen(str);
		size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
		if (chr_count > max_count) chr_count = max_count;

		// Konverter ASCII -> UTF-16
		for (size_t i = 0; i < chr_count; i++)
			_desc_str[1 + i] = str[i];
	}

	// Første ord: lengde (bytes) + type
	_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
	return _desc_str;
}
