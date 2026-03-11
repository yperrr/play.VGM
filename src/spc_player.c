/*
 * spc_player.c -- Playdate SPC player using blargg's snes_spc
 *
 * Uses the C API from spc.h which wraps the C++ SNES_SPC/SPC_DSP internals.
 * The fast DSP variant (~3x faster than accurate) is used for Playdate
 * performance.  Output is native 32000 Hz stereo.
 *
 * SPC file format (SNES-SPC700 Sound File Data v0.30):
 *   0x00   Header string "SNES-SPC700 Sound File Data v0.30" (33 bytes)
 *   0x23   Has ID666 tag (0x1A = yes)
 *   0x2E   Song title (32 bytes, text)
 *   0x4E   Game title (32 bytes, text)
 *   0xB1   Artist     (32 bytes, text)
 *   0x100  64 KB SPC700 RAM
 *   0x10100 128 DSP registers
 *   Total: 0x10200 bytes (66,048)
 */

#include "spc_player.h"
#include "spc.h"
#include <stdlib.h>
#include <string.h>

struct SpcPlayer {
    PlaydateAPI*   pd;
    SNES_SPC*      spc;
    unsigned char* filedata;
    long           filesize;
    int32_t        total_samples;
    char           title[33];
    char           game[33];
};

SpcPlayer* spc_player_open(PlaydateAPI* pd, const char* path)
{
    /* Read the SPC file from the Playdate filesystem */
    SDFile* f = pd->file->open(path, kFileRead | kFileReadData);
    if (!f) {
        pd->system->logToConsole("SPC: failed to open %s", path);
        return NULL;
    }

    pd->file->seek(f, 0, SEEK_END);
    long filesize = pd->file->tell(f);
    pd->file->seek(f, 0, SEEK_SET);

    /* SPC files are exactly 0x10200 bytes (66,048), but some have
     * extended ID666 tags making them slightly larger.  Reject anything
     * clearly too small or absurdly large. */
    if (filesize < 0x10200 || filesize > 0x20000) {
        pd->system->logToConsole("SPC: bad filesize %ld for %s", filesize, path);
        pd->file->close(f);
        return NULL;
    }

    unsigned char* filedata = (unsigned char*)malloc(filesize);
    if (!filedata) { pd->file->close(f); return NULL; }

    if (pd->file->read(f, filedata, (unsigned int)filesize) != (int)filesize) {
        pd->system->logToConsole("SPC: read error for %s", path);
        free(filedata);
        pd->file->close(f);
        return NULL;
    }
    pd->file->close(f);

    /* Create SPC emulator */
    SNES_SPC* spc = spc_new();
    if (!spc) {
        pd->system->logToConsole("SPC: spc_new() failed (out of memory)");
        free(filedata);
        return NULL;
    }

    /* Load the SPC data */
    spc_err_t err = spc_load_spc(spc, filedata, filesize);
    if (err) {
        pd->system->logToConsole("SPC: load error: %s", err);
        spc_delete(spc);
        free(filedata);
        return NULL;
    }

    /* Clear echo buffer -- many SPC files have garbage in echo region
     * which causes a burst of noise at start */
    spc_clear_echo(spc);

    /* Disable surround processing — saves DSP cycles on fast variant */
    spc_disable_surround(spc, 1);

    /* Build player struct */
    SpcPlayer* sp = (SpcPlayer*)malloc(sizeof(SpcPlayer));
    if (!sp) {
        spc_delete(spc);
        free(filedata);
        return NULL;
    }

    sp->pd       = pd;
    sp->spc      = spc;
    sp->filedata = filedata;
    sp->filesize = filesize;

    /* Extract ID666 metadata from the header.
     * Fields are 32-byte null-padded text strings. */
    sp->title[0] = '\0';
    sp->game[0]  = '\0';

    sp->total_samples = 0;

    if (filesize >= 0xD2 && filedata[0x23] == 0x1A) {
        /* Has ID666 tag */
        memcpy(sp->title, filedata + 0x2E, 32);
        sp->title[32] = '\0';
        memcpy(sp->game, filedata + 0x4E, 32);
        sp->game[32] = '\0';

        /* Trim trailing spaces/nulls */
        for (int i = 31; i >= 0 && (sp->title[i] == ' ' || sp->title[i] == '\0'); i--)
            sp->title[i] = '\0';
        for (int i = 31; i >= 0 && (sp->game[i] == ' ' || sp->game[i] == '\0'); i--)
            sp->game[i] = '\0';

        /* Parse song duration from ID666 tag.
         * Offset 0xA9: song length in seconds
         * Offset 0xAC: fade length in milliseconds
         * Two formats exist — text (ASCII digits) and binary (LE integers).
         * Heuristic: if bytes at 0xA9-0xAB are all ASCII digits or NUL, it's text. */
        if (filesize >= 0xB1) {
            unsigned char b0 = filedata[0xA9], b1 = filedata[0xAA], b2 = filedata[0xAB];
            int is_text = ((b0 >= '0' && b0 <= '9') || b0 == '\0') &&
                          ((b1 >= '0' && b1 <= '9') || b1 == '\0') &&
                          ((b2 >= '0' && b2 <= '9') || b2 == '\0');

            int song_secs = 0;
            int fade_ms   = 0;

            if (is_text) {
                /* Text format: 3-char seconds at 0xA9, 5-char fade ms at 0xAC */
                char tmp[6];
                memcpy(tmp, filedata + 0xA9, 3); tmp[3] = '\0';
                song_secs = atoi(tmp);
                memcpy(tmp, filedata + 0xAC, 5); tmp[5] = '\0';
                fade_ms = atoi(tmp);
            } else {
                /* Binary format: uint16 LE seconds at 0xA9, uint32 LE fade ms at 0xAC */
                song_secs = (int)(filedata[0xA9] | (filedata[0xAA] << 8));
                fade_ms   = (int)(filedata[0xAC] | (filedata[0xAD] << 8) |
                                  (filedata[0xAE] << 16) | (filedata[0xAF] << 24));
            }

            if (song_secs > 0 && song_secs < 3600) {
                sp->total_samples = (int32_t)song_secs * 32000
                                  + (int32_t)fade_ms * 32 /* == fade_ms * 32000 / 1000 */;
            }
        }
    }

    pd->system->logToConsole("SPC: opened %s - title=\"%s\" game=\"%s\"",
                             path, sp->title, sp->game);

    return sp;
}

void spc_player_close(SpcPlayer* sp)
{
    if (!sp) return;
    if (sp->spc)    spc_delete(sp->spc);
    free(sp->filedata);
    free(sp);
}

int spc_player_fill(SpcPlayer* sp, int16_t* buf, int n_samples)
{
    /* spc_play() wants a count of individual samples (L+R interleaved),
     * so for n_samples stereo frames we pass n_samples * 2.
     * Our caller treats n_samples as frames (one per channel pair). */
    int sample_count = n_samples * 2;

    spc_err_t err = spc_play(sp->spc, sample_count, (short*)buf);
    if (err) {
        /* Shouldn't happen during normal playback, but fill silence */
        memset(buf, 0, sample_count * sizeof(int16_t));
        sp->pd->system->logToConsole("SPC: play error: %s", err);
        return n_samples;
    }

    return n_samples;
}

int         spc_player_channels    (SpcPlayer* sp) { (void)sp; return 2;     }
int         spc_player_sample_rate (SpcPlayer* sp) { (void)sp; return 32000; }
int32_t     spc_player_total_samples(SpcPlayer* sp){ return sp->total_samples; }
const char* spc_player_title       (SpcPlayer* sp) { return sp->title;       }
const char* spc_player_game        (SpcPlayer* sp) { return sp->game;        }

void spc_player_seek(SpcPlayer* sp, int32_t sample)
{
    if (sample < 0) sample = 0;

    /* SPC has no random-access seek — reload from saved file data and skip */
    spc_load_spc(sp->spc, sp->filedata, sp->filesize);
    spc_clear_echo(sp->spc);
    spc_disable_surround(sp->spc, 1);

    if (sample > 0) {
        /* spc_skip() wants stereo sample count (L+R pairs * 2) */
        spc_skip(sp->spc, sample * 2);
    }
}
