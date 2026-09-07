cmake_minimum_required(VERSION 3.20)
project(airkvm_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# These sources are required to build with no ESP-IDF headers (AGENTS.md §4);
# this target failing to compile is the enforcement of that rule, not a comment.
set(AIRKVM_PURE_SOURCES
    ../../main/keymap.c
    ../../main/protocol.c
    ../../main/hid_state.c
    ../../main/session.c
    ../../main/wake_policy.c
    ../../main/msc_lun.c
    ../../main/media_proto.c
    ../../main/block_cache.c)

include_directories(../../main)
enable_testing()

foreach(suite keymap protocol hid_state session wake_policy msc_lun media_proto
        block_cache)
  add_executable(test_${suite} test_${suite}.c ${AIRKVM_PURE_SOURCES})
  add_test(NAME ${suite} COMMAND test_${suite})
endforeach()

# oled_geometry.h is header-only macros with no hardware dependency (unlike
# display.c, which needs driver/i2c_master.h and is excluded above) — no
# extra sources needed.
add_executable(test_oled_geometry test_oled_geometry.c)
add_test(NAME oled_geometry COMMAND test_oled_geometry)

# canvas.h is header-only and free of esp_ headers for the same reason
# oled_geometry.h is: so the rendering can be checked without a board. The font
# tables are plain data translation units, and both are linked here so the
# suite can compare the two boards' metrics against each other.
add_executable(test_canvas test_canvas.c ../../main/font3x5.c ../../main/font8x16.c)
add_test(NAME canvas COMMAND test_canvas)

# redirect.h is header-only and pure for the same reason keymap.c is: the part
# worth testing is string handling on attacker-controlled input, and that
# should not need a board to exercise.
add_executable(test_redirect test_redirect.c)
add_test(NAME redirect COMMAND test_redirect)

# status.c itself needs FreeRTOS, but status.h's debug-expiry predicate is pure
# and is the part R-006 depends on being right.
add_executable(test_status_debug test_status_debug.c)
add_test(NAME status_debug COMMAND test_status_debug)

# indicator.c needs LEDC/RMT, but the question "what is the LED saying" is pure
# and is now shared by two backends, so it is exactly the part worth pinning.
add_executable(test_indicator test_indicator.c)
add_test(NAME indicator COMMAND test_indicator)
