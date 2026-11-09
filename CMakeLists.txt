cmake_minimum_required(VERSION 3.24)
project(gta2_recomp C OBJC)

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
# Any direct-call target the disassembler still missed logs when it is hit.
target_compile_options(gta2_game PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/include/recomp_stub_hook.h)

add_executable(gta2 MACOSX_BUNDLE
  runtime/src/main.c
  runtime/src/cpu_state.c
  runtime/src/guest_memory.c
  runtime/src/overrides.c
  runtime/src/win32/imports.c
  runtime/src/win32/kernel32.c
  runtime/src/win32/kernel32_file.c
  runtime/src/win32/user32.m
  runtime/src/win32/advapi32.c
  runtime/src/win32/modules.c
  runtime/src/win32/misc.c
  runtime/src/platform/keymap.c
  runtime/src/video/dmavideo.c
  runtime/src/input/dinput.c
  runtime/src/input/gamepad.m
  runtime/src/audio/mixer.c
  runtime/src/audio/miles.c
  runtime/src/video/presenter.m
  runtime/src/render/d3ddll.c
  runtime/src/debug/watch.c
  runtime/src/render3d/world_data.c
  runtime/src/render3d/world3d.m
  runtime/src/render/render_metal.m
  ${GEN}/imports_table.c
  $<TARGET_OBJECTS:gta2_game>)
target_include_directories(gta2 PRIVATE runtime/include runtime/src ${GEN}/gen)
target_compile_options(gta2 PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
set_source_files_properties(runtime/src/render3d/world3d.m runtime/src/win32/user32.m runtime/src/video/presenter.m
  runtime/src/input/gamepad.m
  runtime/src/render/render_metal.m
  PROPERTIES COMPILE_FLAGS -fobjc-arc)
target_link_libraries(gta2 PRIVATE "-framework AppKit" "-framework QuartzCore" "-framework Metal"
  "-framework CoreFoundation" "-framework ImageIO" "-framework UniformTypeIdentifiers"
  "-framework CoreGraphics" "-framework AudioToolbox" "-framework GameController")

# GTA2.app; build/gta2 links to its executable for the command-line scripts.
set_target_properties(gta2 PROPERTIES
  OUTPUT_NAME GTA2
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/mac/Info.plist.in)
add_custom_command(TARGET gta2 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink GTA2.app/Contents/MacOS/GTA2 ${CMAKE_BINARY_DIR}/gta2)
