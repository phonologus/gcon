# gcon-graphics: the display server for gcon. Builds the vendored SDL3,
# Blend2D and AsmJit (unpacked from deps/ by build.sh), the drawing and
# platform library forked from icon-sdl, and the server itself.
cmake_minimum_required(VERSION 3.24)
project(gcon_graphics C CXX)

set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/build/src")

# SDL3: static, video and events only.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(sub AUDIO JOYSTICK HAPTIC HIDAPI CAMERA SENSOR POWER GPU)
   set(SDL_${sub} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${DEPS}/sdl3" sdl3)

# Blend2D with its JIT; AsmJit sits next to it.
set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
set(BLEND2D_NO_INSTALL ON CACHE BOOL "" FORCE)
set(ASMJIT_DIR "${DEPS}/asmjit" CACHE PATH "" FORCE)
add_subdirectory("${DEPS}/blend2d" blend2d)

if(APPLE)
   set(FONTSRC src/fontmac.c)
else()
   set(FONTSRC src/fontlinux.c)
   find_package(Fontconfig REQUIRED)
endif()

# The canvas and platform layers, forked from icon-sdl's src/sdl.
add_library(gfx STATIC src/sdlgfx.c src/canvas.c src/plat.c src/xcolors.c
   ${FONTSRC})
set_property(TARGET gfx PROPERTY C_STANDARD 11)
target_compile_options(gfx PRIVATE -Wall -Wextra)
target_link_libraries(gfx PRIVATE SDL3::SDL3-static blend2d::blend2d)
if(APPLE)
   target_link_libraries(gfx PRIVATE "-framework CoreText"
      "-framework CoreFoundation")
else()
   target_link_libraries(gfx PRIVATE Fontconfig::Fontconfig)

   # SDL silently leaves out video drivers whose development headers are
   # missing; without X11 or Wayland it could open no windows at all.
   if(NOT SDL_X11 AND NOT SDL_WAYLAND)
      message(FATAL_ERROR "SDL3 would be built without X11 and Wayland "
         "support: install the packages listed in linux-packages.txt")
   endif()
endif()

add_executable(gcon-graphics src/main.c)
set_property(TARGET gcon-graphics PROPERTY C_STANDARD 11)
target_compile_options(gcon-graphics PRIVATE -Wall -Wextra)
target_link_libraries(gcon-graphics PRIVATE gfx)
