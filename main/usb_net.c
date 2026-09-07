// See usb_net.h. Unverified on hardware.

#include "usb_net.h"

#include <stdlib.h>
#include <string.h>

#include "dhcpserver/dhcpserver_options.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/esp_netif_net_stack.h"
#include "tinyusb.h"
#include "tinyusb_net.h"

static const char *TAG = "usb_net";

// --- descriptors ------------------------------------------------------------

enum {
  ITF_NCM_CONTROL = 0,
  ITF_NCM_DATA,  // taken by the NCM macro; listed so ITF_COUNT is honest
  ITF_COUNT,
};

#define EPNUM_NCM_NOTIF 0x81
#define EPNUM_NCM_OUT 0x02
#define EPNUM_NCM_IN 0x82

// String indexes. esp_tinyusb owns index 0-3 by convention (language,
// manufacturer, product, serial) and then numbers the class strings in the
// order CDC, MSC, NET interface, MAC — and tinyusb_net_init() writes the
// MAC string into *its* idea of that index (tusb_get_mac_string_id()),
// regardless of the table handed to tinyusb_driver_install(). So the MAC
// index here is derived the same way, and usb_net_start() checks the two
// agree before installing anything: a mismatch would hand the host a NULL
// string where it expects a MAC, which Linux answers by refusing the
// interface with nothing on our side to say why.
#define STRID_ITF (4 + CFG_TUD_CDC + CFG_TUD_MSC)
#define STRID_MAC (STRID_ITF + 1)
// Declared in esp_tinyusb's private usb_descriptors.h; exists whenever
// CFG_TUD_NCM is set, which is the only time this file compiles.
uint8_t tusb_get_mac_string_id(void);

static const char *s_string_desc[STRID_MAC + 1] = {
    [0] = (const char[]){0x09, 0x04},  // English (US)
    [1] = "airkvm",
    [2] = "airkvm setup",
    [3] = "000001",
    // Gaps (the CDC/MSC interface slots) stay NULL; nothing references them.
    [STRID_ITF] = "airkvm setup link",
    // Placeholder; tinyusb_net_init() replaces it with the device MAC.
    [STRID_MAC] = "000000000000",
};

static const uint8_t s_configuration_desc[] = {
    // No remote wakeup: nothing to wake anyone for in setup mode.
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0,
                          TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN, 0, 100),
    // 64 is the full-speed bulk packet size. The MTU is CFG_TUD_NET_MTU
    // (1514 in esp_tinyusb's tusb_config.h), plain Ethernet.
    TUD_CDC_NCM_DESCRIPTOR(ITF_NCM_CONTROL, STRID_ITF, STRID_MAC,
                           EPNUM_NCM_NOTIF, 64, EPNUM_NCM_OUT, EPNUM_NCM_IN, 64,
                           CFG_TUD_NET_MTU),
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // NCM is a two-interface function bound by an IAD, and a host only looks
    // for IADs when the device class says so.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    // Espressif's PID map has no bit for the network class, so 0x4000 —
    // their value for "no mapped class" — is the one their own descriptor
    // would produce for this interface set. It is distinct from the normal
    // image's 0x4006 (HID + MSC) on purpose: hosts cache descriptors per
    // VID/PID, and this device has a completely different shape.
    .idProduct = 0x4000,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// --- the netif ----------------------------------------------------------------

static esp_netif_t *s_netif;
// esp_netif_dhcps_option() keeps the POINTER for option 114 (setup_mode.c
// says why); this must outlive usb_net_start().
static const char s_captive_uri[] = "http://" USB_NET_IP_STR;

static const esp_netif_ip_info_t s_ip_info = {
    .ip = {.addr = ESP_IP4TOADDR(192, 168, 7, 1)},
    // No gateway: this link goes nowhere, and the DHCP router option is
    // suppressed below so the host never learns one either.
    .gw = {.addr = ESP_IP4TOADDR(0, 0, 0, 0)},
    .netmask = {.addr = ESP_IP4TOADDR(255, 255, 255, 0)},
};

// TinyUSB → lwIP. Runs on the TinyUSB task; the copy is what lets that task
// hand the NTB buffer back immediately.
static esp_err_t on_usb_recv(void *buffer, uint16_t len, void *ctx) {
  (void)ctx;
  if (s_netif == NULL) {
    return ESP_OK;  // not up yet; nothing to deliver to
  }
  void *copy = malloc(len);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }
  memcpy(copy, buffer, len);
  return esp_netif_receive(s_netif, copy, len, NULL);
}

static void netif_free_rx(void *h, void *buffer) {
  (void)h;
  free(buffer);
}

// lwIP → TinyUSB. Synchronous: tinyusb_net_send_sync returns once the frame
// has been copied into an NTB, so the caller's buffer is free to go when we
// return. 100 ms is generous for one Ethernet frame on a full-speed link,
// and a host that has stopped draining gets its frames dropped, not queued.
static esp_err_t netif_transmit(void *h, void *buffer, size_t len) {
  (void)h;
  const esp_err_t err =
      tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(100));
  if (err != ESP_OK) {
    ESP_LOGD(TAG, "tx dropped: %s", esp_err_to_name(err));
  }
  return ESP_OK;  // the frame is gone either way; lwIP has no retry to offer
}

static esp_err_t configure_netif(esp_netif_t *netif, const uint8_t lwip_mac[6]);

static esp_err_t start_netif(const uint8_t lwip_mac[6]) {
  const esp_netif_inherent_config_t base = {
      .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
      .ip_info = &s_ip_info,
      .if_key = "usb",
      .if_desc = "usb ncm setup link",
      // Below the softAP (WiFi AP is 10): if both links are ever candidates
      // for the same destination, which they are not, the AP wins.
      .route_prio = 5,
  };
  const esp_netif_driver_ifconfig_t driver = {
      // esp_netif requires a non-NULL handle; NCM is a singleton with no
      // instance to point at.
      .handle = (void *)1,
      .transmit = netif_transmit,
      .driver_free_rx_buffer = netif_free_rx,
  };
  const struct esp_netif_netstack_config stack = {
      .lwip = {.init_fn = ethernetif_init, .input_fn = ethernetif_input},
  };
  const esp_netif_config_t cfg = {
      .base = &base, .driver = &driver, .stack = &stack};

  esp_netif_t *netif = esp_netif_new(&cfg);
  ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "esp_netif_new failed");
  const esp_err_t err = configure_netif(netif, lwip_mac);
  if (err != ESP_OK) {
    esp_netif_destroy(netif);  // never started; nothing else holds it
    return err;
  }
  s_netif = netif;
  esp_netif_action_start(netif, NULL, 0, NULL);
  return ESP_OK;
}

static esp_err_t configure_netif(esp_netif_t *netif,
                                 const uint8_t lwip_mac[6]) {
  ESP_RETURN_ON_ERROR(esp_netif_set_mac(netif, (uint8_t *)lwip_mac), TAG,
                      "set mac");

  // DHCP shape, set while the server is still stopped (AUTOUP starts it in
  // esp_netif_action_start below). Router and DNS options OFF is the whole
  // point of this module (usb_net.h): the host gets an address on this link
  // and no reason to send anything else through it. Option 114 is the
  // captive-portal hint, harmless where ignored.
  uint32_t lease_min = 1;
  uint8_t off = 0;
  ESP_RETURN_ON_ERROR(
      esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME,
                             &lease_min, sizeof(lease_min)),
      TAG, "lease");
  ESP_RETURN_ON_ERROR(
      esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                             ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &off,
                             sizeof(off)),
      TAG, "router option off");
  ESP_RETURN_ON_ERROR(
      esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                             ESP_NETIF_DOMAIN_NAME_SERVER, &off, sizeof(off)),
      TAG, "dns option off");
  const esp_err_t opt = esp_netif_dhcps_option(
      netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
      (void *)s_captive_uri, strlen(s_captive_uri));
  if (opt != ESP_OK) {
    ESP_LOGW(TAG, "captive-portal DHCP option not set (%s)",
             esp_err_to_name(opt));
  }
  return ESP_OK;
}

esp_err_t usb_net_start(void) {
  if (tusb_get_mac_string_id() != STRID_MAC) {
    ESP_LOGE(TAG, "esp_tinyusb numbers the MAC string %u, this table %u",
             tusb_get_mac_string_id(), (unsigned)STRID_MAC);
    return ESP_ERR_INVALID_STATE;
  }

  // Two MACs, both locally administered, derived from the chip's base MAC
  // so two dongles on one machine do not collide: one is the NCM function
  // (what the host's new adapter reports as its own address), the other is
  // lwIP's end of the link. They have to differ — they are the two ends of
  // an Ethernet segment — and they must not equal the WiFi MACs, which are
  // base, base+1 in the last byte: setting the local bit in byte 0 takes
  // care of that.
  uint8_t dev_mac[6];
  ESP_RETURN_ON_ERROR(esp_read_mac(dev_mac, ESP_MAC_BASE), TAG, "base mac");
  dev_mac[0] = (uint8_t)((dev_mac[0] | 0x02) & 0xFE);
  uint8_t lwip_mac[6];
  memcpy(lwip_mac, dev_mac, sizeof(lwip_mac));
  lwip_mac[5] ^= 0x01;

  const tinyusb_config_t cfg = {
      .device_descriptor = &s_device_desc,
      .string_descriptor = s_string_desc,
      .string_descriptor_count =
          sizeof(s_string_desc) / sizeof(s_string_desc[0]),
      .external_phy = false,
      .configuration_descriptor = s_configuration_desc,
  };
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG,
                      "tinyusb_driver_install failed");

  tinyusb_net_config_t net = {
      .on_recv_callback = on_usb_recv,
  };
  memcpy(net.mac_addr, dev_mac, sizeof(net.mac_addr));
  ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net), TAG,
                      "tinyusb_net_init failed");

  ESP_RETURN_ON_ERROR(start_netif(lwip_mac), TAG, "netif");
  ESP_LOGI(TAG, "USB network device up at %s (PID 0x4000, %d itfs)",
           USB_NET_IP_STR, ITF_COUNT);
  return ESP_OK;
}
