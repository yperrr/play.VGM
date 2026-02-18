/**
 * mod_player.h — Playdate tracker module player API (MOD/S3M/XM/IT)
 *
 * The backend is libxmp-lite: a lightweight C library that decodes
 * common tracker module formats into PCM audio.
 *
 * Supported formats: ProTracker MOD, Scream Tracker S3M,
 *                    FastTracker II XM, Impulse Tracker IT
 *
 * Usage (same pattern as sid_player / vgmstream in main.c):
 *   open -> fill (repeatedly) -> close
 */

#ifndef MOD_PLAYER_H
#define MOD_PLAYER_H

#include <stdint.h>
#include "pd_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ModPlayer ModPlayer;

/**
 * Open a tracker module from the Playdate filesystem.
 * Reads the whole file into RAM, passes it to libxmp for parsing.
 *
 * @return non-NULL on success, NULL on failure.
 */
ModPlayer* mod_player_open(PlaydateAPI* pd, const char* path);

/**
 * Free all resources.  Safe to call with NULL.
 */
void mod_player_close(ModPlayer* mp);

/**
 * Render up to `n_samples` of stereo interleaved 16-bit PCM into `buf`.
 * Returns the number of samples actually rendered, or 0 at end of module.
 */
int mod_player_fill(ModPlayer* mp, int16_t* buf, int n_samples);

/** Always returns 2 (stereo output). */
int mod_player_channels(ModPlayer* mp);

/** Always returns 44100. */
int mod_player_sample_rate(ModPlayer* mp);

/**
 * Returns total length in samples (at 44100 Hz).
 * Derived from the module's total time reported by libxmp.
 */
int32_t mod_player_total_samples(ModPlayer* mp);

/** Module title embedded in the file (may be empty). */
const char* mod_player_title(ModPlayer* mp);

/**
 * Seek to the given sample position (converted to ms internally).
 * Approximate — libxmp seeks to the nearest row boundary.
 */
void mod_player_seek(ModPlayer* mp, int32_t sample);

#ifdef __cplusplus
}
#endif

#endif /* MOD_PLAYER_H */
