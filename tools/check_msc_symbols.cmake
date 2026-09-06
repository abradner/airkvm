# Fails the build if esp_tinyusb's tusb_msc_storage.o was linked into the ELF.
#
# That object strongly defines tud_mount_cb, tud_umount_cb and every tud_msc_*
# callback. If it is present, its copies may have won over main's, and the
# symptom of that is not a link error but an unplug that no longer releases
# held keys. Positive control: main's own tud_msc_read10_cb must be present
# whenever MSC is compiled in, so a build with the class silently dropped
# cannot pass as "nothing to check".
#
# Invoked with -DNM=<nm> -DELF=<path> -P from the root CMakeLists.txt.
if(NOT NM OR NOT ELF)
    message(FATAL_ERROR "check_msc_symbols: NM and ELF must be set")
endif()

execute_process(
    COMMAND ${NM} --defined-only ${ELF}
    OUTPUT_VARIABLE syms
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "check_msc_symbols: ${NM} failed on ${ELF}")
endif()

string(FIND "${syms}" " tinyusb_msc_storage_init_sdmmc" espressif_storage)
string(FIND "${syms}" " tud_msc_read10_cb" ours)
string(FIND "${syms}" " tud_umount_cb" umount)

if(NOT espressif_storage EQUAL -1)
    message(FATAL_ERROR
        "esp_tinyusb's tusb_msc_storage.o is linked into ${ELF}. Its strong "
        "tud_mount_cb/tud_umount_cb/tud_msc_* may have displaced airkvm's — an "
        "unplug would then stop releasing held keys. See main/CMakeLists.txt "
        "(WHOLE_ARCHIVE) and main/hid_out_usb.c.")
endif()
if(umount EQUAL -1)
    # The C3 bench build: log HID backend, no TinyUSB at all. Nothing to guard.
    message(STATUS "check_msc_symbols: no USB backend in this image; nothing to check")
elseif(ours EQUAL -1)
    message(STATUS "check_msc_symbols: MSC not compiled in; nothing to check")
else()
    message(STATUS "check_msc_symbols: airkvm's MSC callbacks linked, esp_tinyusb's storage glue absent")
endif()
