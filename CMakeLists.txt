cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

# Default to Release if no build type specified (single-config generators).
# Without this, the SDK's CONFIG-based -O2 flags don't activate.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

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
set(PLAYDATE_GAME_NAME play.VGM)
set(PLAYDATE_GAME_DEVICE play.VGM)

project(${PLAYDATE_GAME_NAME} C ASM)

# ── Directory shortcuts ───────────────────────────────────────────────
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs)

# ── Propagate Cortex-M7 flags to ALL targets (including libraries) ────
# The SDK's playdate.cmake only sets these on the game target.
# Without this, libraries like libxmp and vgmstream compile in ARM mode,
# which the Cortex-M7 cannot execute (UNDEFINSTR hard fault).
if(TOOLCHAIN STREQUAL "armgcc")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 -D__FPU_USED=1")
endif()

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
set(BUILD_V123        OFF CACHE BOOL "" FORCE)

if(EXISTS ${LIBS_DIR}/vgmstream/CMakeLists.txt)
    add_subdirectory(${LIBS_DIR}/vgmstream ${CMAKE_CURRENT_BINARY_DIR}/vgmstream)
else()
    message(WARNING
        "vgmstream not found. Run: git clone https://github.com/vgmstream/vgmstream.git libs/vgmstream")
endif()

# ── SID emulation: cRSID by Hermit (both simulator and device) ───────
#
# cRSID is a lightweight integer-only C64 SID emulator in pure C.
# It uses a unity build (libcRSID.c includes all its own sub-files),
# which is compiled via sid_player_crsid.c.
#
# To enable SID playback, clone cRSID into libs/crsid/:
#   git clone https://github.com/r-moeritz/crsid-by-hermit.git libs/crsid
#
if(EXISTS ${LIBS_DIR}/crsid/libcRSID.c)
    set(SID_SOURCES src/sid_player_crsid.c)
    set(SID_INCDIRS ${LIBS_DIR}/crsid)
else()
    message(WARNING
        "cRSID not found — SID playback disabled.\n"
        "To enable it, run:\n"
        "  git clone https://github.com/r-moeritz/crsid-by-hermit.git libs/crsid")
    set(SID_SOURCES "")
    set(SID_INCDIRS "")
endif()
set(SID_LIBS "")

# ── Tracker module playback: libxmp-lite (MOD/S3M/XM/IT) ─────────────
#
# libxmp-lite is a lightweight C library for decoding tracker modules.
# We build the "lite" variant which supports MOD, S3M, XM, IT only
# (no exotic formats), keeping the binary small for Playdate.
#
# To enable tracker playback, clone libxmp into libs/libxmp/:
#   git clone https://github.com/libxmp/libxmp.git libs/libxmp
#
set(BUILD_LITE    ON  CACHE BOOL "" FORCE)
set(BUILD_STATIC  ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(LIBXMP_DISABLE_DEPACKERS ON CACHE BOOL "" FORCE)
set(LIBXMP_DISABLE_PROWIZARD ON CACHE BOOL "" FORCE)

if(EXISTS ${LIBS_DIR}/libxmp/CMakeLists.txt)
    add_subdirectory(${LIBS_DIR}/libxmp ${CMAKE_CURRENT_BINARY_DIR}/libxmp)
    set(MOD_SOURCES src/mod_player.c)
    set(MOD_INCDIRS ${LIBS_DIR}/libxmp/include)
    set(MOD_LIBS xmp_lite_static)
else()
    message(WARNING
        "libxmp not found — tracker playback disabled.\n"
        "To enable it, run:\n"
        "  git clone https://github.com/libxmp/libxmp.git libs/libxmp")
    set(MOD_SOURCES "")
    set(MOD_INCDIRS "")
    set(MOD_LIBS "")
endif()

# ── Game sources ─────────────────────────────────────────────────────
set(GAME_SRC
    src/main.c
    src/vgm_pd_streamfile.c
    ${SID_SOURCES}
    ${MOD_SOURCES}
)

# ── Create targets BEFORE including playdate_game.cmake ─────────────
if (TOOLCHAIN STREQUAL "armgcc")
    add_executable(${PLAYDATE_GAME_DEVICE}
        ${SDK}/C_API/buildsupport/setup.c
        ${GAME_SRC}
        src/pd_syscalls.c
    )
    target_include_directories(${PLAYDATE_GAME_DEVICE} PRIVATE
        ${SRC_DIR}
        ${LIBS_DIR}/vgmstream/src
        ${LIBS_DIR}/vgmstream/src/base
        ${SID_INCDIRS}
        ${MOD_INCDIRS}
    )
    target_compile_definitions(${PLAYDATE_GAME_DEVICE} PRIVATE
        GAME_NAME="${PLAYDATE_GAME_NAME}"
    )
    set(_dev_libs "")
    if(TARGET libvgmstream)
        list(APPEND _dev_libs libvgmstream)
    endif()
    if(MOD_LIBS)
        list(APPEND _dev_libs ${MOD_LIBS})
    endif()
    if(_dev_libs)
        target_link_libraries(${PLAYDATE_GAME_DEVICE} PRIVATE ${_dev_libs})
    endif()
else()
    add_library(${PLAYDATE_GAME_NAME} SHARED
        ${GAME_SRC}
    )
    target_include_directories(${PLAYDATE_GAME_NAME} PRIVATE
        ${SRC_DIR}
        ${LIBS_DIR}/vgmstream/src
        ${LIBS_DIR}/vgmstream/src/base
        ${SID_INCDIRS}
        ${MOD_INCDIRS}
    )
    target_compile_definitions(${PLAYDATE_GAME_NAME} PRIVATE
        GAME_NAME="${PLAYDATE_GAME_NAME}"
    )
    set(_libs "")
    if(TARGET libvgmstream)
        list(APPEND _libs libvgmstream)
    endif()
    if(SID_LIBS)
        list(APPEND _libs ${SID_LIBS})
    endif()
    if(MOD_LIBS)
        list(APPEND _libs ${MOD_LIBS})
    endif()
    if(_libs)
        target_link_libraries(${PLAYDATE_GAME_NAME} PRIVATE ${_libs})
    endif()
endif()

# ── PDX output goes into target/sim or target/device ─────────────
if(TOOLCHAIN STREQUAL "armgcc")
    set(PRODUCT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/target/device CACHE STRING "Built PDX directory" FORCE)
else()
    set(PRODUCT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/target/sim CACHE STRING "Built PDX directory" FORCE)
endif()

# ── Let SDK set compile flags, packaging, etc. ──────────────────────
include(${SDK}/C_API/buildsupport/playdate_game.cmake)

# ── Override SDK's -O2 with -O3 for the game target on device ────────
# The SDK hardcodes -O2 via target_compile_options.  Adding -O3 after
# makes GCC use it instead (last -O flag wins).  Critical for cRSID
# and vgmstream which need every cycle on the Cortex-M7.
if(TOOLCHAIN STREQUAL "armgcc")
    target_compile_options(${PLAYDATE_GAME_DEVICE} PRIVATE -O3)
    if(TARGET libvgmstream)
        target_compile_options(libvgmstream PRIVATE -O3)
    endif()
    if(TARGET xmp_lite_static)
        target_compile_options(xmp_lite_static PRIVATE -O3)
    endif()
endif()

# ── Clean stale PDX before build, then recreate vgm/ folder ──────────
if(TOOLCHAIN STREQUAL "armgcc")
    set(_PDX_TARGET ${PLAYDATE_GAME_DEVICE})
else()
    set(_PDX_TARGET ${PLAYDATE_GAME_NAME})
endif()

add_custom_command(TARGET ${_PDX_TARGET} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "${PRODUCT_DIR}/${PLAYDATE_GAME_NAME}.pdx"
    COMMENT "Cleaning ${PLAYDATE_GAME_NAME}.pdx/"
)

add_custom_command(TARGET ${_PDX_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${PRODUCT_DIR}/${PLAYDATE_GAME_NAME}.pdx/vgm"
    COMMENT "Creating ${PLAYDATE_GAME_NAME}.pdx/vgm/"
)