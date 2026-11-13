# fantatech: Windows (MSVC or MSYS2), macOS and Linux. SDL3 and SDL3_ttf build from the
# submodules in thirdparty/ and link statically, so nothing is needed from the system.
#
#   git submodule update --init --recursive
#   cmake --preset msvc          (or msys2, macos, linux; see CMakePresets.json)
#   cmake --build --preset msvc-release
#
# Visual Studio: open build/msvc/fantatech.sln (fantatech starts with the repo's mana01\).

cmake_minimum_required(VERSION 3.24)
project(fantatech VERSION 0.1.0 LANGUAGES C)

option(FANTATECH_WERROR "Treat warnings in fantatech's own code as errors (CI does)" OFF)
set(FANTATECH_VERSION "dev" CACHE STRING "Version shown in the title bar and --version (CI passes the release tag)")
# a release tag must match the project version above, so bump both together
if(FANTATECH_VERSION MATCHES "^v[0-9]" AND NOT FANTATECH_VERSION STREQUAL "v${PROJECT_VERSION}")
  message(FATAL_ERROR "tag ${FANTATECH_VERSION} doesn't match project(VERSION ${PROJECT_VERSION}) in CMakeLists.txt")
endif()

# ---- SDL3 and SDL3_ttf, static ------------------------------------------------

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/SDL/CMakeLists.txt")
  message(FATAL_ERROR "thirdparty/SDL is empty: run git submodule update --init --recursive")
endif()

set(BUILD_SHARED_LIBS OFF)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/SDL EXCLUDE_FROM_ALL)

set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)    # FreeType and HarfBuzz from SDL_ttf's own submodules
set(SDLTTF_HARFBUZZ ON CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)   # no SVG glyphs needed
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/SDL_ttf EXCLUDE_FROM_ALL)
# MSVC 14.51 (Visual Studio 2026) miscompiles SDL_ttf at /O2: MS Gothic's small sizes come out
# with the wrong glyphs ("Music" as "Musio", "Fullscreen" as "Rullsoreen"). /O1 draws them right.
if(MSVC)
  target_compile_options(SDL3_ttf-static PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O1>)
endif()
if(MSVC AND TARGET harfbuzz)
  target_compile_options(harfbuzz PRIVATE /wd4312) # its own pointer-size cast, not our code
endif()

# ---- our code -------------------------------------------------------------------

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# C17 and warning-free under MSVC /W4 and gcc/clang -Wall -Wextra. Only our targets get these: the
# dependencies build with their own settings (a global C standard changes how MSVC preprocesses
# FreeType, which then draws broken glyphs).
function(ft_target_options target)
  set_target_properties(${target} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_definitions(${target} PRIVATE FT_VERSION="${FANTATECH_VERSION}")
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 /sdl $<$<BOOL:${FANTATECH_WERROR}>:/WX>
                           $<$<CONFIG:Release>:/Zi /GL /Gy /Oi>)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    # Release keeps a PDB (crash reports) and links whole-program, like the old VS projects did
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF /LTCG>)
    set_property(TARGET ${target} APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:Release>:/LTCG>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra $<$<BOOL:${FANTATECH_WERROR}>:-Werror>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE _FILE_OFFSET_BITS=64) # fseeko under -std=c17
    endif()
  endif()
endfunction()

# Engine core without SDL: archives, decoders, the installer, the script interpreter, assets,
# translations, saves and settings. Used headless by ftconv.
add_library(ftcore STATIC
  src/core/ft_common.c
  src/core/md5.c
  src/vfs/archive.c
  src/vfs/install.c
  src/vfs/iscab.c
  src/vfs/iso9660.c
  src/codec/inflate.c
  src/codec/cp932.c
  src/codec/cp932_table.c
  src/codec/image.c
  src/codec/bmp.c
  src/codec/text.c
  src/codec/wav.c
  src/codec/zdf.c
  src/codec/avi.c
  src/codec/cinepak.c
  src/codec/msadpcm.c
  src/game/assets.c
  src/game/i18n.c
  src/game/save.c
  src/game/config.c
  src/script/expr.c
  src/script/interp.c
  src/script/script.c
)
target_include_directories(ftcore PUBLIC src)
ft_target_options(ftcore)

# Asset tool: list / extract / decode / verify archive entries, scripts, i18n templates.
add_executable(ftconv src/tools/ftconv.c src/tools/png_write.c)
target_link_libraries(ftconv PRIVATE ftcore)
ft_target_options(ftconv)

# The engine. WIN32: no console window when played (see console_open() in main.c).
add_executable(fantatech WIN32
  src/main.c
  src/game/choicemenu.c
  src/game/font.c
  src/game/movie.c
  src/game/runtime.c
  src/game/savepanel.c
  src/game/settingspanel.c
  src/game/titlemenu.c
  src/game/ui.c
  src/tools/png_write.c
)
target_link_libraries(fantatech PRIVATE ftcore SDL3_ttf::SDL3_ttf SDL3::SDL3)
if(WIN32)
  enable_language(RC)
  target_sources(fantatech PRIVATE src/fantatech.rc)
  if(MSVC)
    target_link_options(fantatech PRIVATE /MANIFEST:NO)
  endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # HarfBuzz is C++: carry its runtime along, so the binary needs only glibc (SDL dlopens the rest)
  target_link_options(fantatech PRIVATE -static-libstdc++ -static-libgcc)
endif()
ft_target_options(fantatech)
# a development build finds i18n/ in the source tree, wherever its build folder is
target_compile_definitions(fantatech PRIVATE FT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Visual Studio: F5 runs the game with the repo's mana01\, ftconv verifies it.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT fantatech)
set_target_properties(fantatech PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  VS_DEBUGGER_COMMAND_ARGUMENTS "\"${CMAKE_SOURCE_DIR}/mana01\"")
set_target_properties(ftconv PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  VS_DEBUGGER_COMMAND_ARGUMENTS "verify \"${CMAKE_SOURCE_DIR}/mana01\"")
