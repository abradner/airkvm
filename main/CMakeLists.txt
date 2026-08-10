set(airkvm_srcs
    "airkvm_main.c"
    "net.c"
    "hid_state.c"
    "status.c"
    "display.c"
    "indicator.c"
    "ui.c"
    "keymap.c"
    "protocol.c")

set(airkvm_requires esp_http_server esp_wifi nvs_flash mdns driver esp_timer)

# Exactly one HID backend is compiled in (see main/hid_out.h). The USB backend
# is only offered on targets with native USB OTG, so this never has to guard
# against a C3 pulling in TinyUSB.
if(CONFIG_AIRKVM_HID_BACKEND_USB)
    list(APPEND airkvm_srcs "hid_out_usb.c")
    list(APPEND airkvm_requires esp_tinyusb)
else()
    list(APPEND airkvm_srcs "hid_out_log.c")
endif()

idf_component_register(
    SRCS ${airkvm_srcs}
    INCLUDE_DIRS "."
    # The console is served straight from flash. Adding a file here is all that
    # is needed to ship it; see net.c for how each is routed.
    EMBED_FILES
        "../web/index.html"
        "../web/app.js"
        "../web/style.css"
    REQUIRES ${airkvm_requires})
