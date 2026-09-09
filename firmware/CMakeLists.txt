cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airkvm)

# Post-link proof that airkvm's USB callbacks, not esp_tinyusb's, made it into
# the image. See main/CMakeLists.txt (WHOLE_ARCHIVE) for the hazard this
# guards. It lives here rather than in main/ because the ELF target only exists
# once project() has run. It reads the linker map, which IDF always writes next
# to the ELF, and the generated config header, so it needs no CONFIG_ value in
# this scope. On an image without USB it has nothing to check and says so.
add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DMAP=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
        -DSDKCONFIG_H=${CMAKE_BINARY_DIR}/config/sdkconfig.h
        -P ${CMAKE_SOURCE_DIR}/tools/check_msc_symbols.cmake
    COMMENT "Checking which USB callbacks were linked"
    VERBATIM)
