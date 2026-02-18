/*
 * mod_player.c — Playdate tracker module player using libxmp-lite
 *
 * libxmp-lite is a lightweight C library that decodes common tracker
 * module formats (MOD, S3M, XM, IT) into PCM audio.
 *
 * Repository: https://github.com/libxmp/libxmp
 *
 * Integration notes
 * -----------------
 * We read the module file from the Playdate filesystem into memory,
 * then pass it to libxmp via xmp_load_module_from_memory().
 * libxmp copies the data internally, so we free our buffer immediately.
 *
 * Output is stereo interleaved int16 at 44100 Hz — matching the
 * Playdate's native audio rate (no resampling needed).
 */

#include "mod_player.h"
#include "xmp.h"
#include <stdlib.h>
#include <string.h>

struct ModPlayer {
    PlaydateAPI*  pd;
    xmp_context   ctx;
    int32_t       total_samples;
    int           finished;     /* 1 when module has reached the end */
    char          title[65];
};

ModPlayer* mod_player_open(PlaydateAPI* pd, const char* path)
{
    /* ── Read the module file from the Playdate filesystem ──────────── */
    SDFile* f = pd->file->open(path, kFileRead | kFileReadData);
    if (!f) return NULL;

    pd->file->seek(f, 0, SEEK_END);
    int filesize = pd->file->tell(f);
    pd->file->seek(f, 0, SEEK_SET);

    /* Tracker modules are typically < 2 MB; cap at 4 MB for safety */
    if (filesize <= 0 || filesize > 4 * 1024 * 1024) {
        pd->file->close(f);
        return NULL;
    }

    unsigned char* data = (unsigned char*)malloc(filesize);
    if (!data) { pd->file->close(f); return NULL; }

    if (pd->file->read(f, data, filesize) != filesize) {
        free(data);
        pd->file->close(f);
        return NULL;
    }
    pd->file->close(f);

    /* ── Create libxmp context and load module ─────────────────────── */
    xmp_context ctx = xmp_create_context();
    if (!ctx) { free(data); return NULL; }

    if (xmp_load_module_from_memory(ctx, data, (long)filesize) != 0) {
        xmp_free_context(ctx);
        free(data);
        return NULL;
    }

    /* libxmp copies module data internally — we can free our buffer */
    free(data);

    /* ── Start the player at Playdate's native sample rate ─────────── */
    if (xmp_start_player(ctx, 44100, 0) != 0) {
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return NULL;
    }

    /* ── Build player struct ───────────────────────────────────────── */
    ModPlayer* mp = (ModPlayer*)malloc(sizeof(ModPlayer));
    if (!mp) {
        xmp_end_player(ctx);
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return NULL;
    }

    mp->pd       = pd;
    mp->ctx      = ctx;
    mp->finished = 0;

    /* Get module metadata */
    struct xmp_module_info mi;
    xmp_get_module_info(ctx, &mi);
    strncpy(mp->title, mi.mod->name, 64);
    mp->title[64] = '\0';

    /* Get total duration (in samples at 44100 Hz) */
    struct xmp_frame_info fi;
    xmp_get_frame_info(ctx, &fi);
    mp->total_samples = (int32_t)((int64_t)fi.total_time * 44100 / 1000);

    return mp;
}

void mod_player_close(ModPlayer* mp)
{
    if (!mp) return;
    xmp_end_player(mp->ctx);
    xmp_release_module(mp->ctx);
    xmp_free_context(mp->ctx);
    free(mp);
}

int mod_player_fill(ModPlayer* mp, int16_t* buf, int n_samples)
{
    if (mp->finished) return 0;

    /* libxmp outputs stereo interleaved int16 by default.
     * Size is in bytes: n_samples * 2 channels * 2 bytes per sample. */
    int bytes = n_samples * 2 * (int)sizeof(int16_t);
    int ret = xmp_play_buffer(mp->ctx, buf, bytes, 1);

    if (ret == -XMP_END) {
        mp->finished = 1;
        /* Buffer was filled (possibly with trailing silence) —
         * return it so the last frame of audio is heard. */
        return n_samples;
    }

    return n_samples;
}

int         mod_player_channels     (ModPlayer* mp) { (void)mp; return 2;     }
int         mod_player_sample_rate  (ModPlayer* mp) { (void)mp; return 44100; }
int32_t     mod_player_total_samples(ModPlayer* mp) { return mp->total_samples; }
const char* mod_player_title        (ModPlayer* mp) { return mp->title;        }

void mod_player_seek(ModPlayer* mp, int32_t sample)
{
    int time_ms = (int)((int64_t)sample * 1000 / 44100);
    xmp_seek_time(mp->ctx, time_ms);
    mp->finished = 0;

    /* Reset internal buffer state after seeking */
    xmp_play_buffer(mp->ctx, NULL, 0, 0);
}
