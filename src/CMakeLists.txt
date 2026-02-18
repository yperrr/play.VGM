cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})

if (NOT ${ENVSDK} STREQUAL "")
	file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
	execute_process(
		COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
		COMMAND head -n 1
		COMMAND cut -c9-
		OUTPUT_VARIABLE SDK
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
endif()

if (NOT EXISTS ${SDK})
	message(FATAL_ERROR "SDK Path not found; set ENV value PLAYDATE_SDK_PATH")
	return()
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
set(CMAKE_XCODE_GENERATE_SCHEME TRUE)

# ── Game name (used by playdate_game.cmake) ─────────────────────────
set(PLAYDATE_GAME_NAME VGMPlayer)
set(PLAYDATE_GAME_DEVICE VGMPlayer_DEVICE)

project(${PLAYDATE_GAME_NAME} C ASM)

# ── vgmstream config ───────────────────────────────────────────────
set(USE_FFMPEG        OFF CACHE BOOL "" FORCE)
set(USE_MPEG          OFF CACHE BOOL "" FORCE)
set(USE_VORBIS        OFF CACHE BOOL "" FORCE)
set(USE_SPEEX         OFF CACHE BOOL "" FORCE)
set(USE_ATRAC9        OFF CACHE BOOL "" FORCE)
set(USE_CELT          OFF CACHE BOOL "" FORCE)
set(USE_G719          OFF CACHE BOOL "" FORCE)
set(USE_G7221         OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_CLI         OFF CACHE BOOL "" FORCE)
set(BUILD_AUDACIOUS   OFF CACHE BOOL "" FORCE)
set(BUILD_FB2K        OFF CACHE BOOL "" FORCE)
set(BUILD_WINAMP      OFF CACHE BOOL "" FORCE)
set(BUILD_XMPLAY      OFF CACHE BOOL "" FORCE)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vgmstream/CMakeLists.txt)
    add_subdirectory(vgmstream)
else()
    message(WARNING
        "vgmstream not found. Run: git clone https://github.com/vgmstream/vgmstream.git")
endif()

# ── SID emulation: cRSID by Hermit (both simulator and device) ───────
#
# cRSID is a lightweight integer-only C64 SID emulator in pure C.
# It uses a unity build (libcRSID.c includes all its own sub-files),
# which is compiled via sid_player_crsid.c.
#
# To enable SID playback, clone cRSID into src/crsid/:
#   git clone https://github.com/r-moeritz/crsid-by-hermit.git src/crsid
#
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/crsid/libcRSID.c)
    set(SID_SOURCES sid_player_crsid.c)
    set(SID_INCDIRS ${CMAKE_CURRENT_SOURCE_DIR}/crsid)
else()
    message(WARNING
        "cRSID not found — SID playback disabled.\n"
        "To enable it, run:\n"
        "  git clone https://github.com/r-moeritz/crsid-by-hermit.git src/crsid")
    set(SID_SOURCES "")
    set(SID_INCDIRS "")
endif()
set(SID_LIBS "")

# ── Game sources ─────────────────────────────────────────────────────
set(GAME_SRC
    main.c
    vgm_pd_streamfile.c
    ${SID_SOURCES}
)

# ── Create targets BEFORE including playdate_game.cmake ─────────────
if (TOOLCHAIN STREQUAL "armgcc")
    add_executable(${PLAYDATE_GAME_DEVICE}
        ${SDK}/C_API/buildsupport/setup.c
        ${GAME_SRC}
    )
    target_include_directories(${PLAYDATE_GAME_DEVICE} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vgmstream/src
        ${CMAKE_CURRENT_SOURCE_DIR}/vgmstream/src/base
        ${SID_INCDIRS}
    )
    if(TARGET libvgmstream)
        target_link_libraries(${PLAYDATE_GAME_DEVICE} PRIVATE libvgmstream)
    endif()
else()
    add_library(${PLAYDATE_GAME_NAME} SHARED
        ${GAME_SRC}
    )
    target_include_directories(${PLAYDATE_GAME_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vgmstream/src
        ${CMAKE_CURRENT_SOURCE_DIR}/vgmstream/src/base
        ${SID_INCDIRS}
    )
    set(_libs "")
    if(TARGET libvgmstream)
        list(APPEND _libs libvgmstream)
    endif()
    if(SID_LIBS)
        list(APPEND _libs ${SID_LIBS})
    endif()
    if(_libs)
        target_link_libraries(${PLAYDATE_GAME_NAME} PRIVATE ${_libs})
    endif()
endif()

# ── Let SDK set compile flags, packaging, etc. ──────────────────────
include(${SDK}/C_API/buildsupport/playdate_game.cmake)

# ── Clean stale PDX before build, then recreate vgm/ folder ──────────
if(TOOLCHAIN STREQUAL "armgcc")
    set(_PDX_TARGET ${PLAYDATE_GAME_DEVICE})
else()
    set(_PDX_TARGET ${PLAYDATE_GAME_NAME})
endif()

add_custom_command(TARGET ${_PDX_TARGET} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLAYDATE_GAME_NAME}.pdx"
    COMMENT "Cleaning ${PLAYDATE_GAME_NAME}.pdx/"
)

add_custom_command(TARGET ${_PDX_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLAYDATE_GAME_NAME}.pdx/vgm"
    COMMENT "Creating ${PLAYDATE_GAME_NAME}.pdx/vgm/"
)
