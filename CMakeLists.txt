cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airkvm)

# Post-link proof that airkvm's USB callbacks, not esp_tinyusb's, made it into
# the image. See main/CMakeLists.txt (WHOLE_ARCHIVE) for the hazard this
# guards. It lives here rather than in main/ because the ELF target only exists
# once project() has run. Runs on every build; on one without MSC it has
# nothing to check and says so.
add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM}
        -DELF=$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
        -P ${CMAKE_SOURCE_DIR}/tools/check_msc_symbols.cmake
    COMMENT "Checking which USB callbacks were linked"
    VERBATIM)
