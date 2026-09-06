# Fails the build if esp_tinyusb's tusb_msc_storage.o took part in the link.
#
# That object strongly defines tud_mount_cb, tud_umount_cb and every tud_msc_*
# callback. If it is present, its copies may have won over main's, and the
# symptom of that is not a link error but an unplug that no longer releases
# held keys. The evidence is the linker MAP, not nm: with --gc-sections an
# unreferenced function in that object is discarded, so an nm sentinel is
# absent in exactly the case this guards against. The map's "Archive member
# included" table records every object that entered the link, gc or not.
#
# Positive control: when the generated config says MSC is on, main's own
# msc_out.c.obj must be in the same table, so a build that silently dropped
# the class cannot pass as "nothing to check".
#
# Invoked with -DMAP=<map> -DSDKCONFIG_H=<header> -P from the root
# CMakeLists.txt.
if(NOT MAP OR NOT SDKCONFIG_H)
    message(FATAL_ERROR "check_msc_symbols: MAP and SDKCONFIG_H must be set")
endif()
if(NOT EXISTS "${MAP}")
    message(FATAL_ERROR "check_msc_symbols: no linker map at ${MAP}")
endif()

file(READ "${MAP}" map)
file(READ "${SDKCONFIG_H}" cfg)

string(FIND "${map}" "tusb_msc_storage.c.obj" espressif_storage)
string(FIND "${map}" "msc_out.c.obj" ours)
string(FIND "${map}" "hid_out_usb.c.obj" usb_backend)
string(REGEX MATCH "#define CONFIG_TINYUSB_MSC_ENABLED 1" msc_on "${cfg}")

if(NOT espressif_storage EQUAL -1)
    message(FATAL_ERROR
        "esp_tinyusb's tusb_msc_storage.c.obj entered the link (see ${MAP}). Its "
        "strong tud_mount_cb/tud_umount_cb/tud_msc_* may have displaced airkvm's — "
        "an unplug would then stop releasing held keys. See main/CMakeLists.txt "
        "(WHOLE_ARCHIVE) and main/hid_out_usb.c.")
endif()
if(usb_backend EQUAL -1)
    # The C3 bench build: log HID backend, no TinyUSB at all. Nothing to guard.
    message(STATUS "check_msc_symbols: no USB backend in this image; nothing to check")
elseif(msc_on AND ours EQUAL -1)
    message(FATAL_ERROR
        "CONFIG_TINYUSB_MSC_ENABLED is set but main's msc_out.c.obj is not in the "
        "link: the class was dropped and the check would pass vacuously.")
elseif(msc_on)
    message(STATUS "check_msc_symbols: airkvm's MSC callbacks linked, esp_tinyusb's storage glue absent")
else()
    message(STATUS "check_msc_symbols: MSC not enabled in this image; storage glue absent")
endif()
