cmake_minimum_required(VERSION 3.16)
project(Terrarium VERSION 0.70.05 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TERRARIUM_USE_FLUIDSYNTH "Enable FluidSynth-backed synth output" OFF)

configure_file(
  src/terrarium_version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/terrarium_version.hpp
  @ONLY
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(terrarium
  src/main.cpp
  src/terrarium_core.cpp
  src/terrarium_audio.cpp
  src/terrarium_app.cpp
  src/terrarium_glyphs.cpp
  src/terrarium_midi_names.cpp
  src/terrarium_visuals.cpp
  src/terrarium_ui.cpp
  src/terrarium_render.cpp
  src/terrarium_runtime.cpp
)

target_include_directories(terrarium PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(terrarium PRIVATE
  ${SDL2_LIBRARIES}
)

target_compile_options(terrarium PRIVATE
  ${SDL2_CFLAGS_OTHER}
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(TERRARIUM_USE_FLUIDSYNTH)
  pkg_check_modules(FLUIDSYNTH REQUIRED fluidsynth)
  target_compile_definitions(terrarium PRIVATE USE_FLUIDSYNTH)
  target_include_directories(terrarium PRIVATE
    ${FLUIDSYNTH_INCLUDE_DIRS}
  )
  target_link_libraries(terrarium PRIVATE
    ${FLUIDSYNTH_LIBRARIES}
  )
  target_compile_options(terrarium PRIVATE
    ${FLUIDSYNTH_CFLAGS_OTHER}
  )
endif()
