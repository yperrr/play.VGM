/**
 * sid_player.h — Playdate SID player API (implemented by sid_player_crsid.c)
 *
 * The backend is cRSID by Hermit (Mihaly Horvath): a lightweight integer-only
 * C64 emulator with cycle-accurate 6510 CPU, CIA/VIC timing, proper ADSR and
 * chip-model-accurate filter tables (6581/8580).
 *
 * Usage (same pattern as vgmstream in main.c):
 *   open → fill (repeatedly) → close
 */

#ifndef SID_PLAYER_H
#define SID_PLAYER_H

#include <stdint.h>
#include "pd_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SidPlayer SidPlayer;

/**
 * Open a SID file from the Playdate filesystem.
 * Reads the whole file into RAM, parses the PSID header,
 * and calls the SID init routine for the default sub-song.
 *
 * @return non-NULL on success, NULL on failure.
 */
SidPlayer* sid_player_open(PlaydateAPI* pd, const char* path);

/**
 * Free all resources.  Safe to call with NULL.
 */
void sid_player_close(SidPlayer* sp);

/**
 * Render up to `n_samples` of mono 16-bit PCM into `buf`.
 * SID tunes loop forever, so this always returns `n_samples`.
 */
int sid_player_fill(SidPlayer* sp, int16_t* buf, int n_samples);

/** Always returns 1 (SID chip is mono). */
int sid_player_channels(SidPlayer* sp);

/** Always returns 44100. */
int sid_player_sample_rate(SidPlayer* sp);

/**
 * Returns 0: SID tunes loop indefinitely so there is no fixed length.
 * main.c should skip the end-of-stream check when total_samples == 0.
 */
int32_t sid_player_total_samples(SidPlayer* sp);

/** Null-terminated title from the PSID header (may be empty). */
const char* sid_player_title(SidPlayer* sp);

/** Null-terminated author from the PSID header (may be empty). */
const char* sid_player_author(SidPlayer* sp);

/** Returns 1 if ~2 seconds of near-silence detected (subtune ended). */
int sid_player_is_silent(SidPlayer* sp);

/** Returns total subtune count (1 = no extra subtunes). */
int sid_player_subtune_count(SidPlayer* sp);

/** Switch to subtune n (1-based, clamped). Returns actual subtune index. */
int sid_player_set_subtune(SidPlayer* sp, int n);

#ifdef __cplusplus
}
#endif

#endif /* SID_PLAYER_H */
