/**
 * spc_player.h -- Playdate SPC player API (SNES SPC700 music files)
 *
 * Backend: blargg's snes_spc (fast DSP variant, ~3x faster than accurate).
 * SPC700 CPU + S-DSP with 8 BRR voices, ADSR envelopes, echo/reverb.
 * Native output: 32000 Hz stereo.
 *
 * Usage (same pattern as sid_player / mod_player):
 *   open -> fill (repeatedly) -> close
 */

#ifndef SPC_PLAYER_H
#define SPC_PLAYER_H

#include <stdint.h>
#include "pd_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SpcPlayer SpcPlayer;

/**
 * Open an SPC file from the Playdate filesystem.
 * Reads the whole file into RAM (~66 KB), loads it into the SPC emulator.
 *
 * @return non-NULL on success, NULL on failure.
 */
SpcPlayer* spc_player_open(PlaydateAPI* pd, const char* path);

/**
 * Free all resources.  Safe to call with NULL.
 */
void spc_player_close(SpcPlayer* sp);

/**
 * Render up to n_samples of stereo 16-bit PCM into buf.
 * Output is interleaved stereo (L, R, L, R, ...).
 * SPC tunes loop forever, so this always returns n_samples.
 */
int spc_player_fill(SpcPlayer* sp, int16_t* buf, int n_samples);

/** Returns 2 (SPC output is stereo). */
int spc_player_channels(SpcPlayer* sp);

/** Returns 32000 (native SNES sample rate). */
int spc_player_sample_rate(SpcPlayer* sp);

/** Returns total samples (song + fade) from ID666 tag, or 0 if unknown. */
int32_t spc_player_total_samples(SpcPlayer* sp);

/** Title from the SPC header (may be empty). */
const char* spc_player_title(SpcPlayer* sp);

/** Game name from the SPC header (may be empty). */
const char* spc_player_game(SpcPlayer* sp);

/**
 * Seek to the given sample position (32000 Hz stereo frames).
 * Reloads the SPC file and uses spc_skip() to advance — not instant,
 * but several times faster than real-time playback.
 */
void spc_player_seek(SpcPlayer* sp, int32_t sample);

#ifdef __cplusplus
}
#endif

#endif /* SPC_PLAYER_H */
