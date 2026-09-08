set(airkvm_srcs
    "airkvm_main.c"
    "net.c"
    "wifi.c"
    "session.c"
    "hid_state.c"
    "status.c"
    "ui.c"
    "keymap.c"
    "protocol.c"
    "wake_policy.c"
    "hold_button.c"
    "setup_flag.c"
    "setup_mode.c"
    "provision.c"
    "captive_dns.c"
    "ota.c"
    "logtap.c")

# esp_https_server is required unconditionally even though TLS is a Kconfig
# option: component requirements are resolved during CMake's early-expansion
# pass, where CONFIG_ values are not reliably visible, so a conditional REQUIRES
# silently fails to add the include path and the build dies on a missing header
# with the option plainly enabled. The code is #if-guarded, so linking it when
# TLS is off costs nothing.
#
# esp_https_ota and app_update are required unconditionally for the same
# reason, even though the whole of ota.c — fetch and rollback-confirm both —
# is a no-op when AIRKVM_OTA_TEST_URL is unset (every real build).
#
# esp_lcd is likewise unconditional even though only one board's display
# backend uses it. Unlike esp_tinyusb below there is no manifest entry to fall
# back on — it is an in-tree IDF component, not a managed one — so a
# conditional REQUIRES here would genuinely fail to link rather than silently
# appearing to work.
# sdmmc and esp_driver_sdmmc are required unconditionally for the same reason
# as everything above (gotcha 7): only the LCD147 build compiles msc_src_sd.c,
# and the C3 has no SDMMC host at all, but the requirement has to be visible
# in the early pass. Both components register empty on targets without the
# peripheral.
set(airkvm_requires
    esp_http_server esp_https_server esp_wifi nvs_flash mdns driver esp_timer
    esp_https_ota app_update esp_lcd esp_driver_rmt sdmmc esp_driver_sdmmc)

# Exactly one HID backend is compiled in (see main/hid_out.h). The USB backend
# is only offered on targets with native USB OTG, so this never has to guard
# against a C3 pulling in TinyUSB.
#
# There is deliberately no `list(APPEND airkvm_requires esp_tinyusb)` here:
# TinyUSB is a managed component and reaches the link through
# main/idf_component.yml, whose `rules: if target in [esp32s3, esp32s2]` keeps
# it off the C3. One did sit here from the first commit and never had any
# effect, appearing to work only because the manifest was supplying the
# dependency the whole time.
#
# The reason is not that this if() fails to run — it plainly runs, which is how
# hid_out_usb.c gets selected below. It is that idf_component_register never
# reads its REQUIRES argument for dependencies: those come from the __REQUIRES
# property (component.cmake, __component_set_all_dependencies), filled by a
# separate early-expansion pass that includes no sdkconfig, so CONFIG_ is
# undefined there and the branch is skipped in the only pass that is read.
# Appending here writes to a list nothing looks at again. SRCS have no such
# split — idf_component_register consumes them in the real pass — which is why
# the conditional below works. AGENTS.md §9 gotcha 7.
if(CONFIG_AIRKVM_HID_BACKEND_USB)
    list(APPEND airkvm_srcs "hid_out_usb.c")
else()
    list(APPEND airkvm_srcs "hid_out_log.c")
endif()

# Mass storage rides on the USB backend and is keyed on esp_tinyusb's own
# symbol rather than one of ours: a `select` from an airkvm Kconfig would
# reference a symbol that does not exist on the C3, and Kconfig has no way to
# say "only if defined". msc_lun.c is pure and host-tested; msc_out.c is the
# TinyUSB glue.
if(CONFIG_TINYUSB_MSC_ENABLED)
    list(APPEND airkvm_srcs "msc_lun.c" "msc_out.c" "media_proto.c" "block_cache.c"
        "block_source_browser.c" "media_ws.c")
endif()
# The setup-mode USB network device (main/usb_net.c) is keyed on esp_tinyusb's
# NET choice symbol for the same reason MSC is keyed on theirs above. It is
# only ever started by setup_mode.c; the normal image carries the class
# driver's static NTB buffers (sized in sdkconfig.defaults) and nothing else.
if(CONFIG_TINYUSB_NET_MODE_NCM)
    list(APPEND airkvm_srcs "usb_net.c")
endif()
if(CONFIG_AIRKVM_MSC_SD)
    if(NOT CONFIG_TINYUSB_MSC_ENABLED)
        # Kconfig cannot express this dependency on a symbol the C3 lacks; say
        # it here rather than fail on an undefined msc_out_* at link.
        message(FATAL_ERROR "AIRKVM_MSC_SD needs CONFIG_TINYUSB_MSC_ENABLED=y")
    endif()
    list(APPEND airkvm_srcs "msc_src_sd.c")
endif()

# Exactly one display backend, selected by the board. Conditional SRCS (unlike
# conditional REQUIRES) are consumed in CMake's real pass, where CONFIG_ values
# are visible, which is why this pattern works and the one above does not.
if(CONFIG_AIRKVM_BOARD_S3_LCD147)
    list(APPEND airkvm_srcs "display_st7789.c" "font8x16.c")
else()
    list(APPEND airkvm_srcs "display_ssd1306.c" "font3x5.c")
endif()

# Likewise exactly one indicator backend (see main/indicator.h). An addressable
# LED is a different driver, not a parameter of the PWM one.
if(CONFIG_AIRKVM_BOARD_S3_LCD147)
    list(APPEND airkvm_srcs "indicator_ws2812.c")
else()
    list(APPEND airkvm_srcs "indicator.c")
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
foreach(asset IN ITEMS index.html app.js style.css setup.html)
    list(APPEND web_assets "${WEB_GZ}/${asset}.gz")
endforeach()

# ESP-IDF evaluates this file twice: once in script mode to collect component
# requirements, where add_custom_command does not exist, and once for real.
# Without this guard the first pass fails with "command is not scriptable".
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    foreach(asset IN ITEMS index.html app.js style.css setup.html)
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

# TLS material, embedded when HTTPS is enabled. These are generated by
# tools/make_certs.sh into a gitignored directory, so a fresh clone will not
# build with TLS on until certificates exist — which is deliberate: silently
# falling back to plaintext would defeat the point (risk R-007).
# Certificates go in EMBED_TXTFILES, not EMBED_FILES, and the distinction is
# load-bearing rather than cosmetic: mbedtls_x509_crt_parse requires
# NUL-terminated PEM and TXTFILES is the variant that appends the terminator.
# With EMBED_FILES the parse fails with -0x2180 and every TLS handshake is
# refused, while the port still listens — so it looks like a certificate or
# cipher problem rather than a build one. (The same distinction bites in the
# opposite direction for the gzipped assets, which must NOT gain a stray byte:
# see send_static in net.c.)
set(airkvm_embed_txt "")
if(CONFIG_AIRKVM_TLS)
    list(APPEND airkvm_embed_txt
        "${CMAKE_CURRENT_LIST_DIR}/../certs/device.crt"
        "${CMAKE_CURRENT_LIST_DIR}/../certs/device.key")
endif()

# WHOLE_ARCHIVE is load-bearing, not tidiness. esp_tinyusb's tusb_msc_storage.c
# is compiled whenever CONFIG_TINYUSB_MSC_ENABLED is set, and it defines every
# tud_msc_* callback and tud_mount_cb/tud_umount_cb *strongly* — the same
# symbols main defines. Which copy an ordinary link takes depends on archive
# search order (it happened to take main's when tried on 2026-09-06, which is
# luck, not a property), and one of the losing outcomes is silent: Espressif's
# tud_umount_cb links, the image builds and enumerates, and an unplug no longer
# releases held keys (§2 principle 4). With main's objects all placed before
# any archive is searched, no tud_* reference is left for tusb_msc_storage.o to
# satisfy, so it is never pulled in — and if it somehow were, main's copies
# would collide with it loudly instead of losing quietly.
# tools/check_msc_symbols.cmake proves the object is absent after every link
# (root CMakeLists.txt). AGENTS.md §9.
idf_component_register(
    SRCS ${airkvm_srcs}
    INCLUDE_DIRS "."
    # Symbol names follow the file name: index.html.gz becomes
    # _binary_index_html_gz_start (see net.c).
    EMBED_FILES ${web_assets}
    EMBED_TXTFILES ${airkvm_embed_txt}
    REQUIRES ${airkvm_requires}
    WHOLE_ARCHIVE)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} airkvm_web_assets)
endif()
