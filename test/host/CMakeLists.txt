cmake_minimum_required(VERSION 3.20)
project(airhid_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# These sources are required to build with no ESP-IDF headers (AGENTS.md §4);
# this target failing to compile is the enforcement of that rule, not a comment.
set(AIRHID_PURE_SOURCES
    ../../main/keymap.c
    ../../main/protocol.c
    ../../main/hid_state.c)

include_directories(../../main)
enable_testing()

foreach(suite keymap protocol hid_state)
  add_executable(test_${suite} test_${suite}.c ${AIRHID_PURE_SOURCES})
  add_test(NAME ${suite} COMMAND test_${suite})
endforeach()
