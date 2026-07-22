cmake_minimum_required(VERSION 3.16)
# C is required by the DPF plugin framework (plugin/dpf).
project(Terrarium VERSION 0.70.05 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TERRARIUM_USE_FLUIDSYNTH "Enable FluidSynth-backed synth output" OFF)
option(TERRARIUM_BUILD_PLUGIN "Build the CLAP/VST3/LV2 plugin (requires plugin/dpf checkout)" ON)

configure_file(
  src/terrarium_version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/terrarium_version.hpp
  @ONLY
)

if(WIN32)
  # vcpkg / MSVC: pkg-config resolves the debug SDL2 lib even in Release;
  # use SDL2's CMake config package instead.
  find_package(SDL2 CONFIG REQUIRED)
  set(SDL2_LIBRARIES $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)
  set(SDL2_INCLUDE_DIRS "")
  set(SDL2_CFLAGS_OTHER "")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

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
  src/terrarium_patch.cpp
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

# Real MIDI output (ALSA sequencer port) so Terrarium can play external
# synths/VST hosts. Optional: without it the MidiOut stub is compiled.
if(NOT WIN32)
  pkg_check_modules(ALSA alsa)
endif()
if(ALSA_FOUND)
  target_compile_definitions(terrarium PRIVATE TERRARIUM_HAS_ALSA)
  target_include_directories(terrarium PRIVATE ${ALSA_INCLUDE_DIRS})
  target_link_libraries(terrarium PRIVATE ${ALSA_LIBRARIES})
endif()

# Minimal 200x200-pixel build for very low-power hardware (Raspberry Pi Zero):
# one pixel per world cell into a streaming texture, redrawn only on sim ticks.
add_executable(terrarium-pico
  src/terrarium_pico.cpp
  src/terrarium_core.cpp
)
target_compile_definitions(terrarium-pico PRIVATE TERRA_W=200 TERRA_H=200 SDL_MAIN_HANDLED)
target_include_directories(terrarium-pico PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(terrarium-pico PRIVATE ${SDL2_LIBRARIES})
target_compile_options(terrarium-pico PRIVATE
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

  # FluidSynth's pipewire audio driver requires pw_init() before use.
  pkg_check_modules(PIPEWIRE libpipewire-0.3)
  if(PIPEWIRE_FOUND)
    target_compile_definitions(terrarium PRIVATE TERRARIUM_HAS_PIPEWIRE)
    # SYSTEM: PipeWire headers use C compound literals that trip -Wpedantic.
    target_include_directories(terrarium SYSTEM PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(terrarium PRIVATE ${PIPEWIRE_LIBRARIES})
    target_compile_options(terrarium PRIVATE ${PIPEWIRE_CFLAGS_OTHER})
  endif()
endif()

# ---- Plugin build (CLAP / VST3 / LV2 MIDI generator via DPF) ----
# The plugin reuses the SDL-free sim core + engine and emits the same
# notes/CCs into the host instead of an ALSA port. It shares the standalone's
# patch file (~/.config/terrarium/patch.txt).
if(TERRARIUM_BUILD_PLUGIN)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugin/dpf/CMakeLists.txt)
    add_subdirectory(plugin/dpf)
    dpf_add_plugin(terrarium-vat
      TARGETS clap vst3 lv2
      FILES_DSP
        plugin/TerrariumPlugin.cpp
        src/terrarium_core.cpp
        src/terrarium_audio.cpp
        src/terrarium_patch.cpp
      FILES_UI
        plugin/TerrariumUI.cpp
    )
    target_include_directories(terrarium-vat PUBLIC plugin src)
  else()
    message(WARNING "TERRARIUM_BUILD_PLUGIN is ON but plugin/dpf is missing; "
                    "clone it with: git clone --depth 1 "
                    "https://github.com/DISTRHO/DPF.git plugin/dpf")
  endif()
endif()
