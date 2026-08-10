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

# The console is gzipped at build time and served compressed. This is not a
# micro-optimisation: measured at -86 dBm the uncompressed 16KB of assets took
# anywhere from 0.2s to over 12s to transfer, because every extra packet is
# another chance to need a retry. Compression cuts the packet count by roughly
# two thirds, and it costs the device nothing at runtime — the bytes are stored
# compressed and handed to the browser as-is.
#
# Generated via a custom command rather than at configure time so that editing
# web/ rebuilds the embedded copy. Baking it at configure time would silently
# serve stale assets after every console edit, which is exactly the sort of
# thing that eats an afternoon.
set(WEB_SRC "${CMAKE_CURRENT_LIST_DIR}/../web")
set(WEB_GZ "${CMAKE_CURRENT_BINARY_DIR}/web")
file(MAKE_DIRECTORY "${WEB_GZ}")

set(web_assets "")
foreach(asset IN ITEMS index.html app.js style.css)
    list(APPEND web_assets "${WEB_GZ}/${asset}.gz")
endforeach()

# ESP-IDF evaluates this file twice: once in script mode to collect component
# requirements, where add_custom_command does not exist, and once for real.
# Without this guard the first pass fails with "command is not scriptable".
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    foreach(asset IN ITEMS index.html app.js style.css)
        add_custom_command(
            OUTPUT "${WEB_GZ}/${asset}.gz"
            # -n omits the timestamp so identical input gives identical output,
            # keeping rebuilds reproducible.
            COMMAND gzip -9 -n -c "${WEB_SRC}/${asset}" > "${WEB_GZ}/${asset}.gz"
            DEPENDS "${WEB_SRC}/${asset}"
            COMMENT "Compressing ${asset}"
            VERBATIM)
    endforeach()
    add_custom_target(airkvm_web_assets DEPENDS ${web_assets})
endif()

idf_component_register(
    SRCS ${airkvm_srcs}
    INCLUDE_DIRS "."
    # Symbol names follow the file name: index.html.gz becomes
    # _binary_index_html_gz_start (see net.c).
    EMBED_FILES ${web_assets}
    REQUIRES ${airkvm_requires})

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} airkvm_web_assets)
endif()
