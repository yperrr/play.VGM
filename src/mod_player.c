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
 * Output is int16 at 44100 Hz (mono or stereo depending on open flag)
 * — matching the Playdate's native audio rate (no resampling needed).
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
    int           mono;         /* 1 = mono output, 0 = stereo      */
    char          title[65];
};

ModPlayer* mod_player_open(PlaydateAPI* pd, const char* path, int mono)
{
    /* ── Read the module file from the Playdate filesystem ──────────── */
    SDFile* f = pd->file->open(path, kFileRead | kFileReadData);
    if (!f) {
        pd->system->logToConsole("mod: failed to open %s", path);
        return NULL;
    }

    pd->file->seek(f, 0, SEEK_END);
    int filesize = pd->file->tell(f);
    pd->file->seek(f, 0, SEEK_SET);

    /* Cap at 2 MB — Playdate has ~10 MB usable RAM */
    if (filesize <= 0 || filesize > 2 * 1024 * 1024) {
        pd->system->logToConsole("mod: file too large (%d bytes)", filesize);
        pd->file->close(f);
        return NULL;
    }

    pd->system->logToConsole("mod: loading %s (%d bytes)", path, filesize);

    unsigned char* data = (unsigned char*)pd->system->realloc(NULL, filesize);
    if (!data) {
        pd->system->logToConsole("mod: malloc failed for file buffer");
        pd->file->close(f);
        return NULL;
    }

    if (pd->file->read(f, data, filesize) != filesize) {
        pd->system->realloc(data, 0);
        pd->file->close(f);
        return NULL;
    }
    pd->file->close(f);

    /* ── Create libxmp context and load module ─────────────────────── */
    xmp_context ctx = xmp_create_context();
    if (!ctx) {
        pd->system->logToConsole("mod: xmp_create_context failed");
        pd->system->realloc(data, 0);
        return NULL;
    }

    int err = xmp_load_module_from_memory(ctx, data, (long)filesize);
    if (err != 0) {
        pd->system->logToConsole("mod: xmp_load_module_from_memory failed (%d)", err);
        xmp_free_context(ctx);
        pd->system->realloc(data, 0);
        return NULL;
    }

    /* libxmp copies module data internally — free our buffer */
    pd->system->realloc(data, 0);

    /* ── Start the player at Playdate's native sample rate ─────────── */
    if (xmp_start_player(ctx, 44100, mono ? XMP_FORMAT_MONO : 0) != 0) {
        pd->system->logToConsole("mod: xmp_start_player failed");
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return NULL;
    }

    /* ── Build player struct ───────────────────────────────────────── */
    ModPlayer* mp = (ModPlayer*)pd->system->realloc(NULL, sizeof(ModPlayer));
    if (!mp) {
        pd->system->logToConsole("mod: malloc failed for ModPlayer");
        xmp_end_player(ctx);
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return NULL;
    }

    mp->pd       = pd;
    mp->ctx      = ctx;
    mp->finished = 0;
    mp->mono     = mono;

    /* Get module metadata */
    struct xmp_module_info mi;
    xmp_get_module_info(ctx, &mi);
    strncpy(mp->title, mi.mod->name, 64);
    mp->title[64] = '\0';

    /* Get total duration (in samples at 44100 Hz) */
    struct xmp_frame_info fi;
    xmp_get_frame_info(ctx, &fi);
    mp->total_samples = (int32_t)((int64_t)fi.total_time * 44100 / 1000);

    pd->system->logToConsole("mod: loaded \"%s\" (%d ms, %d ch)",
        mp->title, fi.total_time, mi.mod->chn);

    return mp;
}

void mod_player_close(ModPlayer* mp)
{
    if (!mp) return;
    xmp_end_player(mp->ctx);
    xmp_release_module(mp->ctx);
    xmp_free_context(mp->ctx);
    mp->pd->system->realloc(mp, 0);
}

int mod_player_fill(ModPlayer* mp, int16_t* buf, int n_samples)
{
    if (mp->finished) return 0;

    /* Size in bytes: n_samples * channels * 2 bytes per sample */
    int ch = mp->mono ? 1 : 2;
    int bytes = n_samples * ch * (int)sizeof(int16_t);
    int ret = xmp_play_buffer(mp->ctx, buf, bytes, 1);

    if (ret == -XMP_END) {
        mp->finished = 1;
        return n_samples;
    }

    if (ret < 0) {
        /* Internal error — stop playback */
        mp->finished = 1;
        memset(buf, 0, bytes);
        return 0;
    }

    return n_samples;
}

int         mod_player_channels     (ModPlayer* mp) { return mp->mono ? 1 : 2; }
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
