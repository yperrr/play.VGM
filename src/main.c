/**
 * play.VGM — POC VGM/VGMStream player for Playdate
 *
 * Architecture overview:
 * ─────────────────────
 * vgmstream (C library) decodes game audio formats → raw PCM int16
 * Playdate's addSource() callback feeds PCM samples to the audio engine
 *
 * This POC integrates a stripped-down vgmstream build to decode common
 * game audio formats (ADX, VAG/VAGP, DSP/ADPCM, BRSTM, HCA, etc.)
 * and streams the decoded PCM through Playdate's audio callback system.
 *
 * Constraints addressed:
 *   - Playdate CPU: ARM Cortex-M7 @ 180MHz (no FPU-heavy codecs)
 *   - Playdate RAM: 16MB (enough for most streams)
 *   - Audio: 44100Hz, 16-bit, mono/stereo via addSource callback
 *   - Display: 400×240 1-bit — UI is minimal by design
 *   - Input: D-pad + A/B + crank
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_api.h"

/* ── vgmstream integration ──────────────────────────────────────────────
 * In a real build you would:
 *   1) Clone vgmstream into a subdirectory
 *   2) Build libvgmstream.a for ARM (see CMakeLists.txt)
 *   3) Link against it
 *
 * For this POC we use the public API from libvgmstream.h
 * The key types/functions are:
 *   - libvgmstream_t              : opaque player context
 *   - libvgmstream_open_stream()  : open a file and detect format
 *   - libvgmstream_fill()         : decode N samples into a buffer
 *   - libvgmstream_seek()         : seek to sample position
 *   - libvgmstream_free()         : free resources
 *   - libvgmstream_format_t       : format info (channels, sample_rate, etc.)
 */
#include "libvgmstream.h"
#include "sid_player.h"

/* Playdate filesystem adapter for vgmstream (see vgm_pd_streamfile.c) */
extern void             vgm_pd_streamfile_set_api(PlaydateAPI* pd);
extern libstreamfile_t* vgm_pd_open_streamfile(const char* path);

/* ── Constants ──────────────────────────────────────────────────────── */

#ifndef GAME_NAME
#define GAME_NAME "play.VGM"
#endif

#define PLAYDATE_AUDIO_RATE    44100
#define DECODE_BUF_SAMPLES     512      /* samples per decode call        */
#define VIS_SAMPLES            360      /* one sample per oscilloscope pixel */
#define MAX_PATH               256
#define BROWSER_MAX_FILES      64
#define FONT_HEIGHT            16

/* ── Player state ───────────────────────────────────────────────────── */

typedef enum {
    STATE_BROWSER,      /* file browser / playlist                     */
    STATE_PLAYING,      /* actively decoding + playing                 */
    STATE_PAUSED,       /* paused playback                             */
    STATE_ERROR         /* decode or file error                        */
} PlayerState;

typedef struct {
    /* Playdate SDK handle */
    PlaydateAPI* pd;

    /* vgmstream decoder context (NULL when playing a SID file) */
    libvgmstream_t* vgm;
    libstreamfile_t* libsf;  /* kept alive during playback — vgmstream reads from it */

    /* SID decoder context (NULL when playing a vgmstream file) */
    SidPlayer* sid;

    /* Track title from metadata (SID header) */
    char track_title[128];

    /* Audio output */
    SoundSource* audio_source;

    /* Decoded PCM ring buffer
     * vgmstream outputs interleaved int16 PCM;
     * Playdate's callback wants int16 per-channel buffers.
     * We keep a small circular buffer to bridge the two. */
    int16_t  decode_buf[DECODE_BUF_SAMPLES * 2];  /* stereo worst case  */
    int      ring_read;
    int      ring_write;
    int      ring_count;  /* samples available (per channel) */

    /* Playback info */
    int      channels;
    int      sample_rate;
    int32_t  total_samples;
    int32_t  current_sample;
    int      loop_count;

    /* Resampler state (fixed-point 16.16)
     * step = source_rate * 65536 / 44100; frac accumulates across callbacks */
    uint32_t resample_step;   /* 0 when no resampling needed */
    uint32_t resample_frac;

    /* UI state */
    PlayerState state;
    int         browser_selection;
    int         browser_count;
    char        browser_files[BROWSER_MAX_FILES][MAX_PATH];
    char        current_file[MAX_PATH];
    char        error_msg[128];

    /* Visualization buffer — snapshot of audio output (left channel) */
    int16_t  vis_buf[VIS_SAMPLES];
    int      vis_count;
    int      vis_enabled;     /* 0 = off, 1 = on */
    int      vis_fullscreen;  /* 0 = inline, 1 = fullscreen */

    /* Crank-based volume (0.0 – 1.0) */
    float    volume;

    /* Title scroll state (marquee for long titles) */
    int      title_scroll_x;     /* current pixel offset */
    int      title_scroll_wait;  /* frames to pause before/after scrolling */

    /* System menu items */
    PDMenuItem* menu_vis_toggle;
    PDMenuItem* menu_vis_fs;

    /* Dirty flag for display refresh */
    int      needs_redraw;
} VGMPlayer;

static VGMPlayer g_player;

/* ── Forward declarations ───────────────────────────────────────────── */

static void player_init(PlaydateAPI* pd);
static int  player_open_file(const char* path);
static void player_close(void);
static void player_play(void);
static void player_pause(void);
static void player_stop(void);
static void player_seek(int32_t sample);

static int  audio_callback(void* context, int16_t* left, int16_t* right, int len);
static void decode_fill_buffer(void);

static void browser_scan(const char* directory);
static void browser_draw(void);
static void player_draw(void);
static void vis_fullscreen_draw(void);
static void error_draw(void);

/* ══════════════════════════════════════════════════════════════════════
 * AUDIO CALLBACK — Heart of the player
 * ══════════════════════════════════════════════════════════════════════
 *
 * Called by Playdate's audio engine every render cycle.
 * Must fill `left` (and `right` if stereo) with `len` samples.
 *
 * Strategy:
 *   1. If ring buffer has enough samples → copy them out
 *   2. Otherwise, call vgmstream to decode more, then copy
 *   3. Apply volume scaling from crank position
 *   4. Return 1 if we produced audio, 0 if silent
 */
static int audio_callback(void* context, int16_t* left, int16_t* right, int len)
{
    VGMPlayer* p = (VGMPlayer*)context;

    /* One-shot diagnostic: log what the SDK actually passes us */
    static int logged = 0;
    if (!logged) {
        logged = 1;
        p->pd->system->logToConsole(
            "audio_callback: len=%d stereo=%s ch=%d",
            len, right ? "yes" : "no", p->channels);
    }

    if (p->state != STATE_PLAYING || (!p->vgm && !p->sid)) {
        /* silence */
        memset(left, 0, len * sizeof(int16_t));
        if (right) memset(right, 0, len * sizeof(int16_t));
        return p->state == STATE_PLAYING ? 1 : 0;
    }

    int written = 0;

    while (written < len) {
        /* Refill decode buffer if empty (or too few samples for interpolation) */
        int min_samples = (p->resample_step ? 2 : 1);
        if (p->ring_count < min_samples) {
            decode_fill_buffer();
            if (p->ring_count < min_samples) {
                if (p->sid) {
                    /* SID loops forever — decode_fill_buffer should never
                     * return 0 for SID, but if it does just output silence
                     * and continue (don't stop playback). */
                    memset(left + written, 0, (len - written) * sizeof(int16_t));
                    if (right) memset(right + written, 0, (len - written) * sizeof(int16_t));
                    return 1;
                }
                /* vgmstream end of stream (including files where total_samples
                 * is 0 = unknown length and the decoder ran out of data). */
                memset(left + written, 0, (len - written) * sizeof(int16_t));
                if (right) memset(right + written, 0, (len - written) * sizeof(int16_t));
                p->state = STATE_BROWSER;
                p->needs_redraw = 1;
                return 1;
            }
        }

        /* Copy from ring buffer to output, with optional resampling */
        float vol = p->volume;
        int src_consumed = 0;

        if (p->resample_step == 0) {
            /* No resampling needed (source == 44100 Hz) */
            int to_copy = len - written;
            if (to_copy > p->ring_count) to_copy = p->ring_count;

            if (p->channels == 1) {
                for (int i = 0; i < to_copy; i++) {
                    int idx = (p->ring_read + i) % DECODE_BUF_SAMPLES;
                    int16_t s = (int16_t)(p->decode_buf[idx] * vol);
                    left[written + i] = s;
                    if (right) right[written + i] = s;
                }
            } else {
                for (int i = 0; i < to_copy; i++) {
                    int idx = (p->ring_read + i) % DECODE_BUF_SAMPLES;
                    int16_t l = (int16_t)(p->decode_buf[idx * 2]     * vol);
                    int16_t r = (int16_t)(p->decode_buf[idx * 2 + 1] * vol);
                    left[written + i] = l;
                    if (right) right[written + i] = r;
                }
            }
            src_consumed = to_copy;
            written += to_copy;
        } else {
            /* Resample: step through source at fractional rate */
            uint32_t frac = p->resample_frac;
            uint32_t step = p->resample_step;
            int remaining = len - written;

            for (int i = 0; i < remaining; i++) {
                int src_i = (int)(frac >> 16);
                if (src_i >= p->ring_count) break;  /* need more source data */

                int f = (int)(frac & 0xFFFF);  /* interpolation fraction 0..65535 */
                int idx0 = (p->ring_read + src_i) % DECODE_BUF_SAMPLES;
                int next  = (src_i + 1 < p->ring_count) ? src_i + 1 : src_i;
                int idx1  = (p->ring_read + next) % DECODE_BUF_SAMPLES;

                if (p->channels == 1) {
                    int s = p->decode_buf[idx0] + ((p->decode_buf[idx1] - p->decode_buf[idx0]) * f >> 16);
                    int16_t out = (int16_t)(s * vol);
                    left[written] = out;
                    if (right) right[written] = out;
                } else {
                    int l = p->decode_buf[idx0 * 2]     + ((p->decode_buf[idx1 * 2]     - p->decode_buf[idx0 * 2])     * f >> 16);
                    int r = p->decode_buf[idx0 * 2 + 1] + ((p->decode_buf[idx1 * 2 + 1] - p->decode_buf[idx0 * 2 + 1]) * f >> 16);
                    left[written]  = (int16_t)(l * vol);
                    if (right) right[written] = (int16_t)(r * vol);
                }

                written++;
                frac += step;
            }

            src_consumed = (int)(frac >> 16);
            p->resample_frac = frac - ((uint32_t)src_consumed << 16);
        }

        p->ring_read = (p->ring_read + src_consumed) % DECODE_BUF_SAMPLES;
        p->ring_count -= src_consumed;
        p->current_sample += src_consumed;
    }

    /* Snapshot output for oscilloscope visualization */
    int vis_n = (written < VIS_SAMPLES) ? written : VIS_SAMPLES;
    memcpy(p->vis_buf, left, vis_n * sizeof(int16_t));
    p->vis_count = vis_n;

    p->needs_redraw = 1;
    return 1;
}

/* ── Decode chunk from vgmstream into ring buffer ───────────────────── */

static void decode_fill_buffer(void)
{
    VGMPlayer* p = &g_player;
    int decoded = 0;

    if (p->sid) {
        /* SID path: always produces the requested number of samples */
        decoded = sid_player_fill(p->sid, p->decode_buf, DECODE_BUF_SAMPLES);
    } else if (p->vgm) {
        /*
         * libvgmstream_fill() decodes audio into the provided buffer.
         * It returns the number of samples actually decoded.
         *
         * The buffer receives interleaved int16 PCM:
         *   mono:   [s0, s1, s2, ...]
         *   stereo: [L0, R0, L1, R1, ...]
         */
        int err = libvgmstream_fill(p->vgm, p->decode_buf, DECODE_BUF_SAMPLES);
        if (err >= 0)
            decoded = p->vgm->decoder->buf_samples;
    }

    if (decoded > 0) {
        p->ring_read  = 0;
        p->ring_write = decoded;
        p->ring_count = decoded;
    } else {
        /* End of stream or error */
        p->ring_count = 0;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * FILE MANAGEMENT
 * ══════════════════════════════════════════════════════════════════════ */

static int player_open_file(const char* path)
{
    VGMPlayer* p = &g_player;

    /* Close previous stream if any */
    player_close();

    strncpy(p->current_file, path, MAX_PATH - 1);
    p->track_title[0] = '\0';
    p->title_scroll_x = 0;
    p->title_scroll_wait = 60;  /* pause at start before scrolling */

    /* ── Route .sid files to the built-in SID emulator ── */
    const char* ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".sid") == 0) {
        p->sid = sid_player_open(p->pd, path);
        if (!p->sid) {
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "SID load failed: %s", path);
            p->state = STATE_ERROR;
            return 0;
        }
        p->channels      = sid_player_channels(p->sid);
        p->sample_rate   = sid_player_sample_rate(p->sid);
        p->total_samples = sid_player_total_samples(p->sid);  /* 0 = infinite */
        p->current_sample = 0;
        p->loop_count    = 0;
        p->resample_step = 0;  /* SID outputs at 44100 Hz, no resampling */
        p->resample_frac = 0;

        /* Populate track title for the player screen */
        const char* title  = sid_player_title(p->sid);
        const char* author = sid_player_author(p->sid);
        if (title[0] != '\0') {
            if (author[0] != '\0')
                snprintf(p->track_title, sizeof(p->track_title),
                         "%s / %s", title, author);
            else
                strncpy(p->track_title, title, sizeof(p->track_title) - 1);
        }

        p->pd->system->logToConsole(
            "SID: opened %s - %dch %dHz (looping)",
            path, p->channels, p->sample_rate
        );
        return 1;
    }

    /* ── vgmstream path ── */
    p->pd->system->logToConsole("VGM: opening %s via pd streamfile", path);

    libstreamfile_t* libsf = vgm_pd_open_streamfile(path);
    if (!libsf) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Cannot open: %s", path);
        p->state = STATE_ERROR;
        return 0;
    }

    p->vgm = libvgmstream_init();
    if (!p->vgm) {
        libstreamfile_close(libsf);
        snprintf(p->error_msg, sizeof(p->error_msg), "vgmstream init failed");
        p->state = STATE_ERROR;
        return 0;
    }

    /* Apply default config (no looping, etc.) */
    libvgmstream_config_t cfg = {0};
    cfg.loop_count = 1;
    libvgmstream_setup(p->vgm, &cfg);

    int err = libvgmstream_open_stream(p->vgm, libsf, 0);

    if (err < 0) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Unsupported format: %s", path);
        libvgmstream_free(p->vgm);
        p->vgm = NULL;
        libstreamfile_close(libsf);
        p->state = STATE_ERROR;
        return 0;
    }

    /* Keep libsf alive — vgmstream reads from it during playback */
    p->libsf = libsf;

    /* Read format info */
    const libvgmstream_format_t* fmt = p->vgm->format;
    p->channels      = fmt->channels;
    p->sample_rate   = fmt->sample_rate;
    p->total_samples = fmt->stream_samples;
    p->current_sample = 0;
    p->loop_count    = 0;

    /* Set up resampler if source rate differs from Playdate's 44100 Hz */
    if (p->sample_rate != PLAYDATE_AUDIO_RATE) {
        p->resample_step = ((uint32_t)p->sample_rate << 16) / PLAYDATE_AUDIO_RATE;
        p->resample_frac = 0;
    } else {
        p->resample_step = 0;
        p->resample_frac = 0;
    }

    p->pd->system->logToConsole(
        "VGM: opened %s - %dch %dHz %d samples",
        path, p->channels, p->sample_rate, (int)p->total_samples
    );

    return 1;
}

static void player_close(void)
{
    VGMPlayer* p = &g_player;
    if (p->vgm) {
        libvgmstream_free(p->vgm);
        p->vgm = NULL;
    }
    if (p->libsf) {
        libstreamfile_close(p->libsf);
        p->libsf = NULL;
    }
    if (p->sid) {
        sid_player_close(p->sid);
        p->sid = NULL;
    }
    p->ring_count = 0;
    p->ring_read  = 0;
    p->ring_write = 0;
    p->current_sample = 0;
}

static void player_play(void)
{
    VGMPlayer* p = &g_player;
    if (!p->vgm && !p->sid) return;

    if (!p->audio_source) {
        /* Register audio callback — stereo if file is stereo */
        int stereo = (p->channels > 1) ? 1 : 0;
        p->audio_source = p->pd->sound->addSource(
            audio_callback,
            (void*)p,
            stereo
        );
    }

    p->state = STATE_PLAYING;
    p->needs_redraw = 1;
}

static void player_pause(void)
{
    VGMPlayer* p = &g_player;
    p->state = STATE_PAUSED;
    p->needs_redraw = 1;
}

static void player_stop(void)
{
    VGMPlayer* p = &g_player;
    if (p->audio_source) {
        p->pd->sound->removeSource(p->audio_source);
        p->audio_source = NULL;
    }
    player_close();
    p->state = STATE_BROWSER;
    p->needs_redraw = 1;
}

static void player_seek(int32_t sample)
{
    VGMPlayer* p = &g_player;
    if (!p->vgm) return;

    if (sample < 0) sample = 0;
    if (sample > p->total_samples) sample = p->total_samples;

    libvgmstream_seek(p->vgm, sample);
    p->current_sample = sample;
    p->ring_count = 0;  /* flush buffer */
    p->needs_redraw = 1;
}

/* ══════════════════════════════════════════════════════════════════════
 * FILE BROWSER — scan /vgm/ directory on Playdate filesystem
 * ══════════════════════════════════════════════════════════════════════ */

/* Callback for Playdate's listfiles API */
typedef struct {
    VGMPlayer*  player;
    const char* directory;
} BrowserScanCtx;

static void browser_listfiles_callback(const char* filename, void* userdata)
{
    BrowserScanCtx* ctx = (BrowserScanCtx*)userdata;
    VGMPlayer* pl = ctx->player;
    if (pl->browser_count >= BROWSER_MAX_FILES) return;

    /* Accept files with known game audio extensions */
    const char* ext = strrchr(filename, '.');
    if (!ext) return;

    /* vgmstream formats natively supported without external codec libraries,
     * plus .sid for the built-in SID emulator.
     * Excluded: .at3/.at9 (ATRAC, needs USE_ATRAC9/FFmpeg),
     *           .ogg (Vorbis, needs USE_VORBIS),
     *           .lopus (Opus, needs USE_VORBIS or system Opus). */
    static const char* supported[] = {
        ".adx",  ".aix",  ".brstm", ".bcstm", ".bfstm",
        ".dsp",  ".hca",  ".idsp",  ".vag",   ".vagp",
        ".wem",  ".xwb",  ".fsb",   ".bnk",   ".acb",
        ".awb",  ".txtp", ".str",   ".ss2",   ".ads",
        ".mib",  ".sid",
        NULL
    };

    for (int i = 0; supported[i]; i++) {
        if (strcasecmp(ext, supported[i]) == 0) {
            snprintf(pl->browser_files[pl->browser_count],
                     MAX_PATH, "%s/%s", ctx->directory, filename);
            pl->browser_count++;
            return;
        }
    }
}

static void browser_scan(const char* directory)
{
    VGMPlayer* p = &g_player;
    p->browser_count = 0;
    p->browser_selection = 0;

    /*
     * Playdate filesystem API to list files.
     * We look for common vgmstream-supported extensions.
     */
    BrowserScanCtx ctx = { .player = p, .directory = directory };
    p->pd->file->listfiles(directory, browser_listfiles_callback, (void*)&ctx, 0);

    p->pd->system->logToConsole("VGM: found %d files in %s",
                                 p->browser_count, directory);
}

/* ══════════════════════════════════════════════════════════════════════
 * UI DRAWING — 1-bit 400×240 display
 * ══════════════════════════════════════════════════════════════════════ */

static void browser_draw(void)
{
    VGMPlayer* p = &g_player;
    PlaydateAPI* pd = p->pd;

    pd->graphics->clear(kColorWhite);

    /* Title bar */
    pd->graphics->fillRect(0, 0, 400, 20, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeInverted);
    {
        static const char title[] = GAME_NAME " - Select File";
        pd->graphics->drawText(title, sizeof(title) - 1,
                               kASCIIEncoding, 8, 2);
    }
    pd->graphics->setDrawMode(kDrawModeCopy);

    if (p->browser_count == 0) {
        pd->graphics->drawText("No files found in /vgm/", 23,
                               kASCIIEncoding, 20, 100);
        pd->graphics->drawText("Place game audio files there", 28,
                               kASCIIEncoding, 20, 120);
        return;
    }

    /* File list */
    int visible_start = p->browser_selection - 6;
    if (visible_start < 0) visible_start = 0;
    int visible_end = visible_start + 13;
    if (visible_end > p->browser_count) visible_end = p->browser_count;

    for (int i = visible_start; i < visible_end; i++) {
        int y = 24 + (i - visible_start) * FONT_HEIGHT;

        if (i == p->browser_selection) {
            pd->graphics->fillRect(0, y, 400, FONT_HEIGHT, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeInverted);
        }

        /* Show just the filename, not full path */
        const char* name = strrchr(p->browser_files[i], '/');
        name = name ? name + 1 : p->browser_files[i];
        pd->graphics->drawText(name, strlen(name),
                               kASCIIEncoding, 12, y);

        if (i == p->browser_selection) {
            pd->graphics->setDrawMode(kDrawModeCopy);
        }
    }

    /* Scrollbar */
    if (p->browser_count > 13) {
        int bar_h = (13 * 216) / p->browser_count;
        int bar_y = 24 + (p->browser_selection * 216) / p->browser_count;
        pd->graphics->fillRect(394, bar_y, 4, bar_h, kColorBlack);
    }
}

/* Helper: draw a play (triangle) or pause (two bars) icon.
 * Draws in the current draw mode color at (x, y) within a 10x12 box. */
static void draw_play_icon(PlaydateAPI* pd, int x, int y, LCDColor color) {
    pd->graphics->fillTriangle(x, y, x, y + 11, x + 8, y + 5, color);
}

static void draw_pause_icon(PlaydateAPI* pd, int x, int y, LCDColor color) {
    pd->graphics->fillRect(x, y, 3, 12, color);
    pd->graphics->fillRect(x + 5, y, 3, 12, color);
}

static void player_draw(void)
{
    VGMPlayer* p = &g_player;
    PlaydateAPI* pd = p->pd;
    char info[128];

    pd->graphics->clear(kColorWhite);

    /* ── Header bar (black) ───────────────────────────────────────── */
    pd->graphics->fillRect(0, 0, 400, 20, kColorBlack);

    /* Play/pause icon + status text (left side, white on black) */
    if (p->state == STATE_PLAYING) {
        draw_play_icon(pd, 6, 4, kColorWhite);
        pd->graphics->setDrawMode(kDrawModeInverted);
        pd->graphics->drawText("PLAYING", 7, kASCIIEncoding, 18, 2);
    } else {
        draw_pause_icon(pd, 6, 4, kColorWhite);
        pd->graphics->setDrawMode(kDrawModeInverted);
        pd->graphics->drawText("PAUSED", 6, kASCIIEncoding, 18, 2);
    }

    /* Volume (right side, white on black) */
    snprintf(info, sizeof(info), "Vol: %d%%", (int)(p->volume * 100));
    int vol_w = pd->graphics->getTextWidth(NULL, info, strlen(info),
                                            kASCIIEncoding, 0);
    pd->graphics->drawText(info, strlen(info),
                           kASCIIEncoding, 392 - vol_w, 2);
    pd->graphics->setDrawMode(kDrawModeCopy);

    /* ── Metadata line (right-aligned, first content row) ─────────── */
    const char* file_ext = strrchr(p->current_file, '.');
    const char* file_type = file_ext ? file_ext + 1 : "???";

    snprintf(info, sizeof(info), "%s  %dch  %dHz  %s",
             file_type, p->channels, p->sample_rate,
             p->channels > 1 ? "Stereo" : "Mono");
    /* Uppercase the file type in-place */
    for (char* c = info; *c && *c != ' '; c++) {
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    }
    int meta_w = pd->graphics->getTextWidth(NULL, info, strlen(info),
                                             kASCIIEncoding, 0);
    pd->graphics->drawText(info, strlen(info),
                           kASCIIEncoding, 388 - meta_w, 26);

    /* ── Title (centered, second content row, with marquee) ───────── */
    const char* name;
    if (p->track_title[0] != '\0') {
        name = p->track_title;
    } else {
        name = strrchr(p->current_file, '/');
        name = name ? name + 1 : p->current_file;
    }

    {
        int name_len = (int)strlen(name);
        int text_w = pd->graphics->getTextWidth(NULL, name, name_len,
                                                 kASCIIEncoding, 0);
        const int title_area_w = 376;  /* 400 - 12px margin each side */

        if (text_w <= title_area_w) {
            /* Fits: center horizontally */
            int title_x = (400 - text_w) / 2;
            pd->graphics->drawText(name, name_len,
                                   kASCIIEncoding, title_x, 44);
            p->title_scroll_x = 0;
        } else {
            /* Marquee: scroll left, pause at each end, snap back */
            int max_scroll = text_w - title_area_w;

            if (p->title_scroll_wait > 0) {
                p->title_scroll_wait--;
                if (p->title_scroll_wait == 0 && p->title_scroll_x >= max_scroll) {
                    p->title_scroll_x = 0;
                    p->title_scroll_wait = 60;
                }
            } else {
                p->title_scroll_x += 2;
                if (p->title_scroll_x >= max_scroll) {
                    p->title_scroll_x = max_scroll;
                    p->title_scroll_wait = 60;
                }
            }

            pd->graphics->setClipRect(12, 44, title_area_w, FONT_HEIGHT);
            pd->graphics->drawText(name, name_len, kASCIIEncoding,
                                   12 - p->title_scroll_x, 44);
            pd->graphics->clearClipRect();
        }
    }

    /* ── Centered stack: progress bar + time + waveform ──────────── */
    /* Available zone: y=64 (below title) to y=216 (above controls) */
    {
        const int zone_top = 64, zone_bot = 216;
        const int bar_h = 12, time_h = FONT_HEIGHT, gap = 8;
        const int wave_h = 60;  /* ±30px amplitude */
        int has_bar = (p->total_samples > 0);
        int has_vis = p->vis_enabled;

        /* Compute total stack height */
        int stack_h = time_h;
        if (has_bar)  stack_h += bar_h + gap;
        if (has_vis)  stack_h += gap + wave_h;

        int y = zone_top + (zone_bot - zone_top - stack_h) / 2;

        /* Progress bar */
        if (has_bar) {
            int bar_w = 360;
            int bar_x = (400 - bar_w) / 2;
            pd->graphics->drawRect(bar_x, y, bar_w, bar_h, kColorBlack);
            int fill_w = (int)((float)p->current_sample / p->total_samples * bar_w);
            if (fill_w > bar_w) fill_w = bar_w;
            pd->graphics->fillRect(bar_x, y, fill_w, bar_h, kColorBlack);
            y += bar_h + gap;
        }

        /* Time display */
        int cur_sec = p->current_sample / p->sample_rate;
        if (p->total_samples > 0) {
            int tot_sec = p->total_samples / p->sample_rate;
            snprintf(info, sizeof(info), "%d:%02d / %d:%02d",
                     cur_sec / 60, cur_sec % 60,
                     tot_sec / 60, tot_sec % 60);
        } else {
            snprintf(info, sizeof(info), "%d:%02d / looping",
                     cur_sec / 60, cur_sec % 60);
        }
        int time_w = pd->graphics->getTextWidth(NULL, info, strlen(info),
                                                 kASCIIEncoding, 0);
        pd->graphics->drawText(info, strlen(info),
                               kASCIIEncoding, (400 - time_w) / 2, y);
        y += time_h + gap;

        /* Waveform visualization */
        if (has_vis) {
            int wave_y = y + wave_h / 2;
            int wave_amp = wave_h / 2;
            pd->graphics->drawLine(20, wave_y, 380, wave_y, 1, kColorBlack);

            if (p->state == STATE_PLAYING && p->vis_count > 0) {
                int prev_py = wave_y;
                for (int x = 0; x < 360; x++) {
                    int idx = x * p->vis_count / 360;
                    int16_t sample_val = p->vis_buf[idx];
                    int py = wave_y + (sample_val * wave_amp) / 32768;
                    pd->graphics->drawLine(20 + x - 1, prev_py,
                                           20 + x, py, 1, kColorBlack);
                    prev_py = py;
                }
            }
        }
    }

    /* ── Controls help (mirrors hardware: D-pad, B, A, Crank) ────── */
    {
        const char* help = (p->total_samples > 0)
            ? "<>:Seek  B:Stop  A:Play/Pause  Crank:Vol"
            : "B:Stop  A:Play/Pause  Crank:Vol";
        int help_w = pd->graphics->getTextWidth(NULL, help, strlen(help),
                                                 kASCIIEncoding, 0);
        pd->graphics->drawText(help, strlen(help),
                               kASCIIEncoding, (400 - help_w) / 2, 222);
    }
}

static void vis_fullscreen_draw(void)
{
    VGMPlayer* p = &g_player;
    PlaydateAPI* pd = p->pd;

    pd->graphics->clear(kColorBlack);

    if (p->vis_count <= 0)
        return;

    /* Oscilloscope centered on screen, full width, large amplitude */
    const int wave_y = 120;  /* vertical center of 240px screen */
    const int wave_amp = 100; /* ±100 pixels */
    const int margin = 4;
    const int width = 400 - margin * 2;

    int prev_y = wave_y;
    for (int x = 0; x < width; x++) {
        int idx = x * p->vis_count / width;
        int16_t sample_val = p->vis_buf[idx];
        int y = wave_y + (sample_val * wave_amp) / 32768;
        pd->graphics->drawLine(margin + x - 1, prev_y,
                               margin + x, y, 1, kColorWhite);
        prev_y = y;
    }

    /* Minimal overlay: play/pause icon + time */
    if (p->state == STATE_PLAYING) {
        draw_play_icon(pd, 6, 226, kColorWhite);
    } else {
        draw_pause_icon(pd, 6, 226, kColorWhite);
    }

    char overlay[64];
    int cur_sec = p->current_sample / p->sample_rate;
    if (p->total_samples > 0) {
        int tot_sec = p->total_samples / p->sample_rate;
        snprintf(overlay, sizeof(overlay), "%d:%02d / %d:%02d",
                 cur_sec / 60, cur_sec % 60, tot_sec / 60, tot_sec % 60);
    } else {
        snprintf(overlay, sizeof(overlay), "%d:%02d", cur_sec / 60, cur_sec % 60);
    }
    pd->graphics->setDrawMode(kDrawModeInverted);
    pd->graphics->drawText(overlay, strlen(overlay),
                           kASCIIEncoding, 18, 224);
    pd->graphics->setDrawMode(kDrawModeCopy);
}

static void error_draw(void)
{
    VGMPlayer* p = &g_player;
    PlaydateAPI* pd = p->pd;

    pd->graphics->clear(kColorWhite);
    pd->graphics->fillRect(0, 0, 400, 20, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeInverted);
    pd->graphics->drawText("ERROR", 5, kASCIIEncoding, 8, 2);
    pd->graphics->setDrawMode(kDrawModeCopy);

    pd->graphics->drawText(p->error_msg, strlen(p->error_msg),
                           kASCIIEncoding, 20, 80);
    pd->graphics->drawText("Press B to go back", 18,
                           kASCIIEncoding, 20, 120);
}

/* ══════════════════════════════════════════════════════════════════════
 * INPUT HANDLING
 * ══════════════════════════════════════════════════════════════════════ */

static void handle_input(void)
{
    VGMPlayer* p = &g_player;
    PlaydateAPI* pd = p->pd;

    PDButtons pushed, current;
    pd->system->getButtonState(&current, &pushed, NULL);

    /* Crank → volume control */
    float crank_change = pd->system->getCrankChange();
    if (crank_change != 0.0f) {
        p->volume += crank_change / 360.0f;
        if (p->volume < 0.0f) p->volume = 0.0f;
        if (p->volume > 1.0f) p->volume = 1.0f;
        p->needs_redraw = 1;
    }

    switch (p->state) {
    case STATE_BROWSER:
        if (pushed & kButtonUp) {
            if (p->browser_selection > 0) p->browser_selection--;
            p->needs_redraw = 1;
        }
        if (pushed & kButtonDown) {
            if (p->browser_selection < p->browser_count - 1)
                p->browser_selection++;
            p->needs_redraw = 1;
        }
        if (pushed & kButtonA) {
            if (p->browser_count > 0) {
                if (player_open_file(p->browser_files[p->browser_selection])) {
                    player_play();
                }
            }
        }
        break;

    case STATE_PLAYING:
        if (pushed & kButtonA) {
            player_pause();
        }
        if (pushed & kButtonB) {
            player_stop();
        }
        /* D-pad left/right → seek ±5 seconds (only for finite tracks) */
        if (p->total_samples > 0) {
            if (pushed & kButtonLeft) {
                player_seek(p->current_sample - p->sample_rate * 5);
            }
            if (pushed & kButtonRight) {
                player_seek(p->current_sample + p->sample_rate * 5);
            }
        }
        break;

    case STATE_PAUSED:
        if (pushed & kButtonA) {
            player_play();
        }
        if (pushed & kButtonB) {
            player_stop();
        }
        if (p->total_samples > 0) {
            if (pushed & kButtonLeft) {
                player_seek(p->current_sample - p->sample_rate * 5);
            }
            if (pushed & kButtonRight) {
                player_seek(p->current_sample + p->sample_rate * 5);
            }
        }
        break;

    case STATE_ERROR:
        if (pushed & kButtonB) {
            p->state = STATE_BROWSER;
            p->needs_redraw = 1;
        }
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * PLAYDATE ENTRY POINTS
 * ══════════════════════════════════════════════════════════════════════ */

/* ── System menu callbacks ───────────────────────────────────────────── */

static void menu_vis_toggle(void* userdata) {
    VGMPlayer* p = (VGMPlayer*)userdata;
    PDMenuItem* item = (PDMenuItem*)p->menu_vis_toggle;
    p->vis_enabled = p->pd->system->getMenuItemValue(item);
    p->needs_redraw = 1;
}

static void menu_vis_fullscreen(void* userdata) {
    VGMPlayer* p = (VGMPlayer*)userdata;
    PDMenuItem* item = (PDMenuItem*)p->menu_vis_fs;
    p->vis_fullscreen = p->pd->system->getMenuItemValue(item);
    p->needs_redraw = 1;
}

static void player_init(PlaydateAPI* pd)
{
    memset(&g_player, 0, sizeof(VGMPlayer));
    g_player.pd     = pd;
    g_player.state  = STATE_BROWSER;
    g_player.volume = 0.8f;
    g_player.vis_enabled = 1;
    g_player.needs_redraw = 1;

    /* Initialise Playdate filesystem adapter for vgmstream */
    vgm_pd_streamfile_set_api(pd);

    /* System menu items */
    g_player.menu_vis_toggle = pd->system->addCheckmarkMenuItem(
        "Visualizer", 1, menu_vis_toggle, &g_player);
    g_player.menu_vis_fs = pd->system->addCheckmarkMenuItem(
        "Fullscreen Viz", 0, menu_vis_fullscreen, &g_player);

    /* Scan for audio files */
    browser_scan("vgm");
}

static int update(void* userdata)
{
    VGMPlayer* p = &g_player;

    handle_input();

    /* Only redraw when needed (saves CPU) */
    if (p->needs_redraw) {
        int is_playing = (p->state == STATE_PLAYING || p->state == STATE_PAUSED);
        if (is_playing && p->vis_enabled && p->vis_fullscreen) {
            vis_fullscreen_draw();
        } else {
            switch (p->state) {
            case STATE_BROWSER:  browser_draw(); break;
            case STATE_PLAYING:  /* fallthrough */
            case STATE_PAUSED:   player_draw();  break;
            case STATE_ERROR:    error_draw();   break;
            }
        }
        p->needs_redraw = 0;
        return 1;  /*
                    * returning 1 tells Playdate we
                    * updated the display
                    */
    }

    /* During playback, redraw periodically for progress bar */
    if (p->state == STATE_PLAYING) {
        p->needs_redraw = 1;
    }

    return 0;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    switch (event) {
    case kEventInit:
        pd->system->setUpdateCallback(update, NULL);
        player_init(pd);
        break;

    case kEventTerminate:
        player_stop();
        break;

    case kEventPause:
        if (g_player.state == STATE_PLAYING)
            player_pause();
        break;

    case kEventResume:
        /* Resume playback that was paused when the system menu opened */
        if (g_player.vgm || g_player.sid)
            player_play();
        break;

    default:
        break;
    }

    return 0;
}
