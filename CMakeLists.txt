cmake_minimum_required(VERSION 3.24)
project(gta2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)

set(GEN ${CMAKE_SOURCE_DIR}/generated)
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${GEN}/gen/*.c)
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "generated/gen is empty: run scripts/translate.sh first")
endif()

# Translated game code: huge, machine-written; warnings are noise.
add_library(gta2_game OBJECT ${GEN_SOURCES})
target_include_directories(gta2_game PRIVATE runtime/include ${GEN}/gen)
target_compile_options(gta2_game PRIVATE -w -O1 -fno-strict-aliasing)

add_executable(gta2
  runtime/src/main.c
  runtime/src/cpu_state.c
  runtime/src/guest_memory.c
  runtime/src/overrides.c
  runtime/src/win32/imports.c
  runtime/src/win32/kernel32.c
  runtime/src/win32/kernel32_file.c
  runtime/src/win32/user32.c
  runtime/src/win32/misc.c
  ${GEN}/imports_table.c
  $<TARGET_OBJECTS:gta2_game>)
target_include_directories(gta2 PRIVATE runtime/include runtime/src ${GEN}/gen)
target_compile_options(gta2 PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
