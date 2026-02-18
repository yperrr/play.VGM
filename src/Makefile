# ══════════════════════════════════════════════════════════════════════
# play.VGM — Makefile for simulator (Windows) and device
#
# Run from this directory (src/) with PLAYDATE_SDK_PATH set:
#   export PLAYDATE_SDK_PATH=/mnt/c/Users/YourName/Documents/PlaydateSDK
#   make          # simulator build (produces play.VGM.pdx)
#   make device   # ARM device build
# ══════════════════════════════════════════════════════════════════════

SDK       ?= $(PLAYDATE_SDK_PATH)
GAME_NAME = play.VGM
PRODUCT   = $(GAME_NAME).pdx

# ── vgmstream paths ────────────────────────────────────────────────
VGM_DIR = vgmstream
VGM_SRC = $(VGM_DIR)/src

# ── Codec flags — disable all heavy codecs ─────────────────────────
UDEFS = \
	-DGAME_NAME=\"$(GAME_NAME)\" \
	-DVGM_USE_FFMPEG=0 \
	-DVGM_USE_MPEG=0 \
	-DVGM_USE_VORBIS=0 \
	-DVGM_USE_SPEEX=0 \
	-DVGM_USE_ATRAC9=0 \
	-DVGM_USE_CELT=0 \
	-DVGM_USE_G719=0 \
	-DVGM_USE_G7221=0

# ── Include paths ──────────────────────────────────────────────────
UINCLUDE = \
	-I. \
	-Icrsid \
	-I$(VGM_SRC) \
	-I$(VGM_SRC)/base \
	-I$(VGM_SRC)/coding \
	-I$(VGM_SRC)/layout \
	-I$(VGM_SRC)/meta \
	-I$(VGM_SRC)/util

# ── Game sources ───────────────────────────────────────────────────
GAME_FILES = \
	main.c \
	vgm_pd_streamfile.c \
	sid_player_crsid.c

# ── vgmstream core sources (minimal ADPCM-only subset) ────────────
VGM_FILES = \
	$(VGM_SRC)/base/api_decode_open.c \
	$(VGM_SRC)/base/api_decode_play.c \
	$(VGM_SRC)/base/api_libsf.c \
	$(VGM_SRC)/base/decode.c \
	$(VGM_SRC)/base/render.c \
	$(VGM_SRC)/base/mixing.c \
	$(VGM_SRC)/base/plugins.c \
	$(VGM_SRC)/base/seek.c \
	$(VGM_SRC)/base/streamfile.c \
	$(VGM_SRC)/coding/adx_decoder.c \
	$(VGM_SRC)/coding/ngc_dsp_decoder.c \
	$(VGM_SRC)/coding/psx_decoder.c \
	$(VGM_SRC)/coding/pcm_decoder.c \
	$(VGM_SRC)/coding/ima_decoder.c \
	$(VGM_SRC)/coding/msadpcm_decoder.c \
	$(VGM_SRC)/meta/adx_header.c \
	$(VGM_SRC)/meta/ngc_dsp_std.c \
	$(VGM_SRC)/meta/ps2_vag.c \
	$(VGM_SRC)/meta/brstm.c \
	$(VGM_SRC)/meta/riff.c \
	$(VGM_SRC)/formats.c

# ── SRC must be the complete list before including common.mk ───────
SRC = $(GAME_FILES) $(VGM_FILES)

# ── Playdate build system ──────────────────────────────────────────
include $(SDK)/C_API/buildsupport/common.mk

.PHONY: help
help:
	@echo "play.date ${GAME_NAME}"
	@echo ""
	@echo "Run from this directory (src/) with PLAYDATE_SDK_PATH set."
	@echo ""
	@echo "Targets:"
	@echo "  make          — build for simulator (Windows DLL via MinGW)"
	@echo "  make device   — build for Playdate hardware (ARM)"
	@echo "  make clean    — remove build artifacts"
