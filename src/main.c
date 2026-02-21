/**
 * play.VGM — POC VGM player for Playdate
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
#include "mod_player.h"

/* Playdate filesystem adapter for vgmstream (see vgm_pd_streamfile.c) */
extern void             vgm_pd_streamfile_set_api(PlaydateAPI* pd);
extern libstreamfile_t* vgm_pd_open_streamfile(const char* path);

/* ── Constants ──────────────────────────────────────────────────────── */

#ifndef GAME_NAME
#define GAME_NAME "play.VGM"
#endif

#define PLAYDATE_AUDIO_RATE    44100
#define RING_BUF_SAMPLES       16384    /* ring buffer capacity (samples) */
#define DECODE_CHUNK           512      /* samples per decode call        */
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

    /* Tracker module decoder context (NULL when not playing a tracker file) */
    ModPlayer* mod;

    /* Track title from metadata (SID header) */
    char track_title[128];

    /* Audio output */
    SoundSource* audio_source;

    /* Lock-free SPSC ring buffer: producer (update/decode) writes ring_write,
     * consumer (audio callback) writes ring_read.  Each index is only ever
     * modified by one thread, eliminating the race condition that caused
     * stuttering with the old shared ring_count approach.
     * One slot is always kept empty to distinguish full from empty. */
    int16_t  ring_buf[RING_BUF_SAMPLES * 2];   /* stereo worst case    */
    int16_t  decode_tmp[DECODE_CHUNK * 2];      /* temp decode output   */
    volatile int ring_read;    /* consumer (audio callback) owns this   */
    volatile int ring_write;   /* producer (decode_fill_ring) owns this */
    int      stream_ended;     /* 1 when decoder has no more data       */

    /* Playback info */
    int      channels;         /* output channels (1 if force_mono) */
    int      source_channels;  /* file's native channel count       */
    int      sample_rate;
    int32_t  total_samples;
    int32_t  current_sample;

    /* Resampler state (fixed-point 16.16)
     * step = source_rate * 65536 / 44100; frac accumulates across callbacks */
    uint32_t resample_step;   /* 0 when no resampling needed */
    uint32_t resample_frac;

    /* UI state */
    PlayerState state;
    int         browser_selection;
    int         browser_count;
    char        browser_files[BROWSER_MAX_FILES][MAX_PATH];
    int         browser_is_dir[BROWSER_MAX_FILES]; /* 0=file, 1=subdir, 2=parent nav ("../") */
    char        current_dir[MAX_PATH];              /* directory currently being browsed */
    char        current_file[MAX_PATH];
    int         current_song_index;  /* index in browser_files for current song */
    char        error_msg[128];

    /* Playback modes */
    int         repeat_mode;    /* 0=off, 1=track, 2=all */
    int         shuffle_enabled; /* 0=off, 1=on */
    int         shuffled_indices[BROWSER_MAX_FILES];
    int         shuffled_count;

    /* Previous/next song display with scrolling */
    char        prev_title[128];
    int         prev_scroll_x;
    int         prev_scroll_wait;
    char        next_title[128];
    int         next_scroll_x;
    int         next_scroll_wait;

    /* Visualization buffer — snapshot of audio output (left channel) */
    int16_t  vis_buf[VIS_SAMPLES];
    int      vis_count;
    int      vis_enabled;     /* 0 = off, 1 = on */
    int      vis_fullscreen;  /* 0 = inline, 1 = fullscreen */

    /* Title scroll state (marquee for long titles) */
    int      title_scroll_x;     /* current pixel offset */
    int      title_scroll_wait;  /* frames to pause before/after scrolling */

    /* Button bar state */
    struct {
        int x, y, w, h;  /* position and size */
    } buttons[7];  /* 0=prev, 1=seek_back, 2=play_pause, 3=seek_fwd, 4=next, 5=shuffle, 6=repeat */
    int selected_button;  /* 0-6 = currently selected button */

    /* System menu items */
    PDMenuItem* menu_vis_toggle;
    PDMenuItem* menu_vis_fs;
    PDMenuItem* menu_mono;
    PDMenuItem* menu_repeat;
    PDMenuItem* menu_shuffle;
    int      menu_items_created;  /* 1 = menu items are currently visible, 0 = hidden */

    /* Audio mode */
    int      force_mono;     /* 1 = mono output (default, better perf), 0 = stereo */

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

/* Menu callback functions */
static void menu_vis_toggle(void* userdata);
static void menu_vis_fullscreen(void* userdata);
static void menu_mono_toggle(void* userdata);
static void menu_repeat_cycle(void* userdata);
static void menu_shuffle_toggle(void* userdata);

/* Playback mode helpers */
static void shuffle_files(VGMPlayer* p);
static int get_next_song_index(VGMPlayer* p);
static void update_prev_next_display(VGMPlayer* p);

static inline int ring_avail(VGMPlayer* p) {
    return (p->ring_write - p->ring_read + RING_BUF_SAMPLES) % RING_BUF_SAMPLES;
}

static int  audio_callback(void* context, int16_t* left, int16_t* right, int len);
static void decode_fill_ring(void);
static void player_prefill_audio(float budget);

static void browser_scan(const char* directory);
static void browser_draw(void);
static void player_draw(void);
static void draw_button_bar(PlaydateAPI* pd, VGMPlayer* p);
static void vis_fullscreen_draw(void);
static void error_draw(void);

/* ══════════════════════════════════════════════════════════════════════
 * AUDIO CALLBACK — reads from pre-filled ring buffer only
 * ══════════════════════════════════════════════════════════════════════
 *
 * Called by Playdate's audio engine every render cycle.
 * Must fill `left` (and `right` if stereo) with `len` samples.
 *
 * IMPORTANT: This callback must be fast.  All decoding happens in
 * update() via player_prefill_audio().  The callback only copies
 * from the ring buffer; if the buffer is empty, it outputs silence.
 */
static int audio_callback(void* context, int16_t* left, int16_t* right, int len)
{
    VGMPlayer* p = (VGMPlayer*)context;

    if (p->state != STATE_PLAYING || (!p->vgm && !p->sid && !p->mod)) {
        memset(left, 0, len * sizeof(int16_t));
        if (right) memset(right, 0, len * sizeof(int16_t));
        return p->state == STATE_PLAYING ? 1 : 0;
    }

    int written = 0;
    int avail = ring_avail(p);

    if (avail <= 0) {
        /* Buffer underrun — output silence, will be refilled in update() */
        memset(left, 0, len * sizeof(int16_t));
        if (right) memset(right, 0, len * sizeof(int16_t));
        return 1;
    }

    if (p->resample_step == 0) {
        /* No resampling (source == 44100 Hz) */
        int to_copy = len;
        if (to_copy > avail) to_copy = avail;

        if (p->channels == 1) {
            for (int i = 0; i < to_copy; i++) {
                int idx = (p->ring_read + i) % RING_BUF_SAMPLES;
                int16_t s = p->ring_buf[idx];
                left[i] = s;
                if (right) right[i] = s;
            }
        } else {
            for (int i = 0; i < to_copy; i++) {
                int idx = (p->ring_read + i) % RING_BUF_SAMPLES;
                left[i]  = p->ring_buf[idx * 2];
                if (right) right[i] = p->ring_buf[idx * 2 + 1];
            }
        }

        /* Zero-fill remainder if buffer didn't have enough */
        if (to_copy < len) {
            memset(left + to_copy, 0, (len - to_copy) * sizeof(int16_t));
            if (right) memset(right + to_copy, 0, (len - to_copy) * sizeof(int16_t));
        }

        p->ring_read = (p->ring_read + to_copy) % RING_BUF_SAMPLES;
        p->current_sample += to_copy;
        written = len;
    } else {
        /* Resample: step through source at fractional rate */
        uint32_t frac = p->resample_frac;
        uint32_t step = p->resample_step;
        int src_consumed = 0;

        for (int i = 0; i < len; i++) {
            int src_i = (int)(frac >> 16);
            if (src_i >= avail - 1) break;  /* need more source data */

            int f = (int)(frac & 0xFFFF);
            int idx0 = (p->ring_read + src_i) % RING_BUF_SAMPLES;
            int idx1 = (p->ring_read + src_i + 1) % RING_BUF_SAMPLES;

            if (p->channels == 1) {
                int s = p->ring_buf[idx0] + ((p->ring_buf[idx1] - p->ring_buf[idx0]) * f >> 16);
                left[written] = (int16_t)s;
                if (right) right[written] = (int16_t)s;
            } else {
                int l = p->ring_buf[idx0 * 2]     + ((p->ring_buf[idx1 * 2]     - p->ring_buf[idx0 * 2])     * f >> 16);
                int r = p->ring_buf[idx0 * 2 + 1] + ((p->ring_buf[idx1 * 2 + 1] - p->ring_buf[idx0 * 2 + 1]) * f >> 16);
                left[written]  = (int16_t)l;
                if (right) right[written] = (int16_t)r;
            }

            written++;
            frac += step;
        }

        src_consumed = (int)(frac >> 16);
        p->resample_frac = frac - ((uint32_t)src_consumed << 16);
        p->ring_read = (p->ring_read + src_consumed) % RING_BUF_SAMPLES;
        p->current_sample += src_consumed;

        /* Zero-fill if we couldn't produce enough output */
        if (written < len) {
            memset(left + written, 0, (len - written) * sizeof(int16_t));
            if (right) memset(right + written, 0, (len - written) * sizeof(int16_t));
        }
    }

    /* Snapshot for oscilloscope visualization (throttled) */
    static int vis_skip = 0;
    if (++vis_skip >= 4) {
        vis_skip = 0;
        int vis_n = (written < VIS_SAMPLES) ? written : VIS_SAMPLES;
        memcpy(p->vis_buf, left, vis_n * sizeof(int16_t));
        p->vis_count = vis_n;
        p->needs_redraw = 1;
    }
    return 1;
}

/* ── Decode one chunk and append to ring buffer ─────────────────────── */


static void decode_fill_ring(void)
{
    VGMPlayer* p = &g_player;
    /* Reserve one slot to distinguish full from empty */
    int space = RING_BUF_SAMPLES - 1 - ring_avail(p);
    if (space < DECODE_CHUNK) return;  /* ring buffer full enough */

    int decoded = 0;

    if (p->sid) {
        decoded = sid_player_fill(p->sid, p->decode_tmp, DECODE_CHUNK);
    } else if (p->mod) {
        decoded = mod_player_fill(p->mod, p->decode_tmp, DECODE_CHUNK);
    } else if (p->vgm) {
        int err = libvgmstream_fill(p->vgm, p->decode_tmp, DECODE_CHUNK);
        if (err >= 0)
            decoded = p->vgm->decoder->buf_samples;
    }

    if (decoded <= 0) {
        p->stream_ended = 1;
        return;
    }

    /* Downmix stereo → mono in-place when force_mono is active */
    if (p->source_channels > 1 && p->channels == 1) {
        for (int i = 0; i < decoded; i++)
            p->decode_tmp[i] = (p->decode_tmp[i * 2] + p->decode_tmp[i * 2 + 1]) / 2;
    }

    /* Append decoded samples to ring buffer, handling wrap-around */
    int write_pos = p->ring_write;
    int ch = (p->channels <= 1) ? 1 : 2;
    int first = RING_BUF_SAMPLES - write_pos;
    if (first > decoded) first = decoded;
    int second = decoded - first;

    memcpy(&p->ring_buf[write_pos * ch], p->decode_tmp, first * ch * sizeof(int16_t));
    if (second > 0)
        memcpy(&p->ring_buf[0], &p->decode_tmp[first * ch], second * ch * sizeof(int16_t));

    p->ring_write = (write_pos + decoded) % RING_BUF_SAMPLES;
}

/* ── Pre-fill ring buffer from update() — keeps audio fed ───────────── */

/* Decode time budgets (seconds).  At 50 fps each frame is 20 ms.
 * On non-draw frames the full frame is available for decoding.
 * On draw frames we must leave headroom for the display update. */
#define DECODE_BUDGET_NODRAW_S  0.018f  /* 18 ms — non-draw frames */
#define DECODE_BUDGET_DRAW_S    0.010f  /* 10 ms — draw frames     */

static void player_prefill_audio(float budget)
{
    VGMPlayer* p = &g_player;
    if (p->state != STATE_PLAYING) return;
    if (!p->vgm && !p->sid && !p->mod) return;

    /* Decode until the ring buffer is ≥75 % full, the stream ends,
     * or we exhaust the time budget — whichever comes first. */
    int target = RING_BUF_SAMPLES * 3 / 4;
    p->pd->system->resetElapsedTime();
    while (ring_avail(p) < target && !p->stream_ended) {
        decode_fill_ring();
        if (p->pd->system->getElapsedTime() >= budget)
            break;
    }

    /* If stream ended and ring buffer is empty, handle repeat/auto-play logic */
    if (p->stream_ended && ring_avail(p) <= 0 && !p->sid) {
        /* Repeat mode: track = 1, all = 2, off = 0 */
        if (p->repeat_mode == 1) {
            /* Repeat current track */
            if (player_open_file(p->current_file)) {
                player_play();
            } else {
                p->state = STATE_BROWSER;
                p->needs_redraw = 1;
            }
        } else if (p->repeat_mode == 2 || (p->repeat_mode == 0 && p->current_song_index >= 0)) {
            /* Play next song (if available) */
            int next_idx = get_next_song_index(p);
            if (next_idx >= 0 && next_idx < p->browser_count) {
                p->current_song_index = next_idx;
                if (player_open_file(p->browser_files[next_idx])) {
                    update_prev_next_display(p);
                    player_play();
                } else {
                    p->state = STATE_BROWSER;
                    p->needs_redraw = 1;
                }
            } else {
                /* No next song, return to browser */
                p->state = STATE_BROWSER;
                p->needs_redraw = 1;
            }
        } else {
            /* Repeat off and no next song, return to browser */
            p->state = STATE_BROWSER;
            p->needs_redraw = 1;
        }
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
        p->source_channels = sid_player_channels(p->sid);
        p->channels      = p->source_channels;
        p->sample_rate   = sid_player_sample_rate(p->sid);
        p->total_samples = sid_player_total_samples(p->sid);  /* 0 = infinite */
        p->current_sample = 0;
        /* Use the fast non-resampling path when source rate matches output */
        if (p->sample_rate != PLAYDATE_AUDIO_RATE) {
            p->resample_step = ((uint32_t)p->sample_rate << 16) / PLAYDATE_AUDIO_RATE;
        } else {
            p->resample_step = 0;
        }
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

    /* ── Route tracker modules to libxmp ── */
    if (ext && (strcasecmp(ext, ".mod") == 0 ||
                strcasecmp(ext, ".xm")  == 0 ||
                strcasecmp(ext, ".it")  == 0 ||
                strcasecmp(ext, ".s3m") == 0)) {
        p->mod = mod_player_open(p->pd, path, p->force_mono);
        if (!p->mod) {
            snprintf(p->error_msg, sizeof(p->error_msg),
                     "Tracker load failed: %s", path);
            p->state = STATE_ERROR;
            return 0;
        }
        p->source_channels = mod_player_channels(p->mod);
        p->channels      = p->source_channels;
        p->sample_rate   = mod_player_sample_rate(p->mod);
        p->total_samples = mod_player_total_samples(p->mod);
        p->current_sample = 0;
        p->resample_step = 0;  /* libxmp outputs at 44100 Hz, no resampling */
        p->resample_frac = 0;

        const char* title = mod_player_title(p->mod);
        if (title[0] != '\0')
            strncpy(p->track_title, title, sizeof(p->track_title) - 1);

        p->pd->system->logToConsole(
            "MOD: opened %s - %dch %dHz %d samples",
            path, p->channels, p->sample_rate, (int)p->total_samples
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
    p->source_channels = fmt->channels;
    p->channels      = (p->force_mono && fmt->channels > 1) ? 1 : fmt->channels;
    p->sample_rate   = fmt->sample_rate;
    p->total_samples = fmt->stream_samples;
    p->current_sample = 0;

    /* Set up resampler if source rate differs from Playdate's 44100 Hz */
    if (p->sample_rate != PLAYDATE_AUDIO_RATE) {
        p->resample_step = ((uint32_t)p->sample_rate << 16) / PLAYDATE_AUDIO_RATE;
        p->resample_frac = 0;
    } else {
        p->resample_step = 0;
        p->resample_frac = 0;
    }

    p->pd->system->logToConsole(
        "VGM: opened %s - %dch->%dch %dHz %d samples",
        path, p->source_channels, p->channels, p->sample_rate, (int)p->total_samples
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
    if (p->mod) {
        mod_player_close(p->mod);
        p->mod = NULL;
    }
    p->ring_read     = 0;
    p->ring_write    = 0;
    p->stream_ended  = 0;
    p->current_sample = 0;
}

static void player_play(void)
{
    VGMPlayer* p = &g_player;
    if (!p->vgm && !p->sid && !p->mod) return;

    if (!p->audio_source) {
        /* Register audio callback — stereo if file is stereo */
        int stereo = (p->channels > 1) ? 1 : 0;
        p->audio_source = p->pd->sound->addSource(
            audio_callback,
            (void*)p,
            stereo
        );
    }

    /* Show player control menu items when starting playback */
    if (!p->menu_items_created) {
        /* ── Display options (main menu) ── */
        p->menu_vis_toggle = p->pd->system->addCheckmarkMenuItem(
            "Display: Visualizer", 1, menu_vis_toggle, p);
        p->menu_vis_fs = p->pd->system->addCheckmarkMenuItem(
            "Display: Fullscreen", 0, menu_vis_fullscreen, p);
        p->menu_mono = p->pd->system->addCheckmarkMenuItem(
            "Audio: Mono Output", 1, menu_mono_toggle, p);
        
        /* ── Playing Mode options ── */
        p->menu_repeat = p->pd->system->addOptionsMenuItem(
            "Playing: Repeat",
            (const char** const)&(const char*[]){"Off", "Single", "All"},
            3, menu_repeat_cycle, p);
        p->menu_shuffle = p->pd->system->addOptionsMenuItem(
            "Playing: Shuffle",
            (const char** const)&(const char*[]){"Off", "Directory", "All"},
            3, menu_shuffle_toggle, p);
        
        p->menu_items_created = 1;
    }

    p->state = STATE_PLAYING;
    p->stream_ended = 0;
    /* Disable auto-lock while playing so screen doesn't sleep */
    p->pd->system->setAutoLockDisabled(1);
    /* Higher refresh rate = more update() calls = more decode opportunities.
     * 50 fps gives ~20 ms per frame instead of ~33 ms at 30 fps. */
    p->pd->display->setRefreshRate(50);
    /* Ring buffer will be filled gradually by update() — no blocking
     * prefill here to avoid stalling on slow decoders (cRSID). */
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
    /* Hide player control menu items when returning to browser */
    if (p->menu_items_created) {
        p->pd->system->removeMenuItem(p->menu_vis_toggle);
        p->pd->system->removeMenuItem(p->menu_vis_fs);
        p->pd->system->removeMenuItem(p->menu_mono);
        p->pd->system->removeMenuItem(p->menu_repeat);
        p->pd->system->removeMenuItem(p->menu_shuffle);
        p->menu_items_created = 0;
    }
    /* Re-enable auto-lock when stopping */
    p->pd->system->setAutoLockDisabled(0);
    player_close();
    p->pd->display->setRefreshRate(30);  /* restore normal rate for browser */
    p->state = STATE_BROWSER;
    p->needs_redraw = 1;
}

static void player_seek(int32_t sample)
{
    VGMPlayer* p = &g_player;

    if (sample < 0) sample = 0;
    if (p->total_samples > 0 && sample > p->total_samples)
        sample = p->total_samples;

    if (p->vgm) {
        libvgmstream_seek(p->vgm, sample);
    } else if (p->mod) {
        mod_player_seek(p->mod, sample);
    } else {
        return;
    }

    p->current_sample = sample;
    p->ring_read     = 0;  /* flush buffer */
    p->ring_write    = 0;
    p->stream_ended  = 0;
    p->needs_redraw  = 1;
}

/* ══════════════════════════════════════════════════════════════════════
 * FILE BROWSER — scan /vgm/ directory on Playdate filesystem
 * ══════════════════════════════════════════════════════════════════════ */

/* Callback for Playdate's listfiles API */
typedef struct {
    VGMPlayer*  player;
    const char* directory;
    /* Pointer to caller-allocated pending_dirs buffer (avoids a large
     * stack allocation — 64×256 bytes would overflow on Playdate). */
    char (*pending_dirs)[MAX_PATH];
    int  pending_dir_count;
} BrowserScanCtx;

static const char* const BROWSER_SUPPORTED_EXT[] = {
    ".adx",  ".aix",  ".brstm", ".bcstm", ".bfstm",
    ".dsp",  ".hca",  ".idsp",  ".vag",   ".vagp",
    ".wem",  ".xwb",  ".fsb",   ".bnk",   ".acb",
    ".awb",  ".txtp", ".str",   ".ss2",   ".ads",
    ".mib",  ".sid",
    ".mod",  ".xm",   ".it",   ".s3m",
    NULL
};

/* Quick check: does a directory contain at least one supported audio file? */
static void browser_check_audio_callback(const char* filename, void* userdata)
{
    int* found = (int*)userdata;
    if (*found) return;
    const char* ext = strrchr(filename, '.');
    if (!ext) return;
    for (int i = 0; BROWSER_SUPPORTED_EXT[i]; i++) {
        if (strcasecmp(ext, BROWSER_SUPPORTED_EXT[i]) == 0) {
            *found = 1;
            return;
        }
    }
}

static int browser_dir_has_audio(PlaydateAPI* pd, const char* dir_path)
{
    int found = 0;
    pd->file->listfiles(dir_path, browser_check_audio_callback, &found, 0);
    return found;
}

static void browser_listfiles_callback(const char* filename, void* userdata)
{
    BrowserScanCtx* ctx = (BrowserScanCtx*)userdata;
    VGMPlayer* pl = ctx->player;
    if (pl->browser_count >= BROWSER_MAX_FILES) return;

    /* Playdate signals directories with a trailing slash on the filename.
     * Use that to detect dirs — no stat() needed (stat inside a listfiles
     * callback corrupts the SDK's directory iterator). */
    int flen = (int)strlen(filename);
    int is_dir = (flen > 0 && filename[flen - 1] == '/');

    if (is_dir) {
        /* Build clean path (no trailing slash) and defer audio check */
        if (ctx->pending_dir_count < BROWSER_MAX_FILES) {
            char* dst = ctx->pending_dirs[ctx->pending_dir_count++];
            snprintf(dst, MAX_PATH, "%s/%.*s", ctx->directory, flen - 1, filename);
        }
        return;
    }

    /* Accept files with known game audio extensions */
    const char* ext = strrchr(filename, '.');
    if (!ext) return;

    for (int i = 0; BROWSER_SUPPORTED_EXT[i]; i++) {
        if (strcasecmp(ext, BROWSER_SUPPORTED_EXT[i]) == 0) {
            snprintf(pl->browser_files[pl->browser_count],
                     MAX_PATH, "%s/%s", ctx->directory, filename);
            pl->browser_is_dir[pl->browser_count] = 0;
            pl->browser_count++;
            return;
        }
    }
}

static void browser_scan(const char* directory)
{
    VGMPlayer* p = &g_player;

    /* Copy directory immediately — the caller may pass a pointer into
     * p->browser_files[], which we overwrite below when adding "../". */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", directory);
    directory = dir;

    p->browser_count = 0;
    p->browser_selection = 0;
    strncpy(p->current_dir, directory, sizeof(p->current_dir) - 1);

    /* Add "../" parent navigation entry if not at the root "vgm" directory */
    if (strcmp(directory, "vgm") != 0) {
        char parent_path[MAX_PATH];
        strncpy(parent_path, directory, sizeof(parent_path) - 1);
        char* last_slash = strrchr(parent_path, '/');
        if (last_slash) {
            *last_slash = '\0';
        } else {
            strncpy(parent_path, "vgm", sizeof(parent_path) - 1);
        }
        strncpy(p->browser_files[0], parent_path, MAX_PATH - 1);
        p->browser_is_dir[0] = 2;  /* parent nav */
        p->browser_count = 1;
    }

    /*
     * Playdate filesystem API to list files.
     * Directories are collected first; audio check happens after listfiles
     * returns so we never make a nested (non-reentrant) listfiles call.
     * pending_dirs is static to avoid a ~16 KB stack allocation.
     */
    static char pending_dirs[BROWSER_MAX_FILES][MAX_PATH];
    BrowserScanCtx ctx = { .player = p, .directory = directory,
                           .pending_dirs = pending_dirs, .pending_dir_count = 0 };
    p->pd->file->listfiles(directory, browser_listfiles_callback, (void*)&ctx, 0);

    /* Now check each candidate directory for audio content */
    for (int i = 0; i < ctx.pending_dir_count && p->browser_count < BROWSER_MAX_FILES; i++) {
        if (browser_dir_has_audio(p->pd, ctx.pending_dirs[i])) {
            snprintf(p->browser_files[p->browser_count], MAX_PATH, "%s", ctx.pending_dirs[i]);
            p->browser_is_dir[p->browser_count] = 1;
            p->browser_count++;
        }
    }

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

        char display_name[MAX_PATH + 2];
        if (p->browser_is_dir[i] == 2) {
            /* Parent navigation entry */
            strncpy(display_name, "../", sizeof(display_name) - 1);
        } else if (p->browser_is_dir[i] == 1) {
            /* Subdirectory: show just the dirname with trailing slash */
            const char* dname = strrchr(p->browser_files[i], '/');
            dname = dname ? dname + 1 : p->browser_files[i];
            snprintf(display_name, sizeof(display_name), "%s/", dname);
        } else {
            /* Regular file: show just the filename */
            const char* fname = strrchr(p->browser_files[i], '/');
            fname = fname ? fname + 1 : p->browser_files[i];
            strncpy(display_name, fname, sizeof(display_name) - 1);
        }

        pd->graphics->drawText(display_name, strlen(display_name),
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

    /* Play/pause icon + status text for Shuffle/Repeat (left side, white on black) */
    draw_play_icon(pd, 6, 4, kColorWhite);
    
    /* Display Shuffle and/or Repeat status */
    pd->graphics->setDrawMode(kDrawModeInverted);
    const char* status_text = NULL;
    if (p->shuffle_enabled && p->repeat_mode == 2) {
        status_text = "Shuffle + Repeat";
    } else if (p->shuffle_enabled && p->repeat_mode == 1) {
        status_text = "Shuffle + Repeat (1)";
    } else if (p->shuffle_enabled) {
        status_text = "Shuffle";
    } else if (p->repeat_mode == 2) {
        status_text = "Repeat";
    } else if (p->repeat_mode == 1) {
        status_text = "Repeat (1)";
    }
    
    if (status_text) {
        pd->graphics->drawText(status_text, strlen(status_text), kASCIIEncoding, 18, 2);
    }

    /* Metadata (right side, white on black) */
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
                           kASCIIEncoding, 392 - meta_w, 2);
    pd->graphics->setDrawMode(kDrawModeCopy);

    /* ── Previous/Next song display (top corners) ────────────────── */
    {
        const int title_max_w = 80;        /* increased from 60 for larger display */
        const int y_prev_next = 24;        /* moved down slightly for better spacing */
        const int label_gap = 5;           /* space between label and title */

        /* Previous song (left side) */
        if (p->prev_title[0] != '\0') {
            int prev_label_w = pd->graphics->getTextWidth(NULL, "Prev:", 5,
                                                           kASCIIEncoding, 0);
            pd->graphics->drawText("Prev:", 5, kASCIIEncoding, 6, y_prev_next);
            int prev_title_x = 6 + prev_label_w + label_gap;  /* label + gap */

            int prev_len = (int)strlen(p->prev_title);
            int prev_w = pd->graphics->getTextWidth(NULL, p->prev_title, prev_len,
                                                     kASCIIEncoding, 0);

            if (prev_w <= title_max_w) {
                pd->graphics->drawText(p->prev_title, prev_len,
                                       kASCIIEncoding, prev_title_x, y_prev_next);
                p->prev_scroll_x = 0;
            } else {
                int max_scroll = prev_w - title_max_w;
                if (p->prev_scroll_wait > 0) {
                    p->prev_scroll_wait--;
                    if (p->prev_scroll_wait == 0 && p->prev_scroll_x >= max_scroll) {
                        p->prev_scroll_x = 0;
                        p->prev_scroll_wait = 60;
                    }
                } else {
                    p->prev_scroll_x += 2;
                    if (p->prev_scroll_x >= max_scroll) {
                        p->prev_scroll_x = max_scroll;
                        p->prev_scroll_wait = 60;
                    }
                }
                pd->graphics->setClipRect(prev_title_x, 24, title_max_w, FONT_HEIGHT);
                pd->graphics->drawText(p->prev_title, prev_len, kASCIIEncoding,
                                       prev_title_x - p->prev_scroll_x, y_prev_next);
                pd->graphics->clearClipRect();
            }
        }

        /* Next song (right side) */
        if (p->next_title[0] != '\0') {
            /* Draw "Next:" label with proper spacing from title */
            int next_label_w = pd->graphics->getTextWidth(NULL, "Next:", 5,
                                                           kASCIIEncoding, 0);
            int next_label_x = 400 - 6 - title_max_w - next_label_w - label_gap;
            pd->graphics->drawText("Next:", 5, kASCIIEncoding, next_label_x, y_prev_next);

            int next_len = (int)strlen(p->next_title);
            int next_w = pd->graphics->getTextWidth(NULL, p->next_title, next_len,
                                                     kASCIIEncoding, 0);
            int next_title_left = next_label_x + next_label_w + label_gap;  /* label + gap */
            int next_right = 400 - 6;  /* right edge with 6px margin */

            if (next_w <= title_max_w) {
                int next_x = next_right - next_w;
                pd->graphics->drawText(p->next_title, next_len,
                                       kASCIIEncoding, next_x, y_prev_next);
                p->next_scroll_x = 0;
            } else {
                int max_scroll = next_w - title_max_w;
                if (p->next_scroll_wait > 0) {
                    p->next_scroll_wait--;
                    if (p->next_scroll_wait == 0 && p->next_scroll_x >= max_scroll) {
                        p->next_scroll_x = 0;
                        p->next_scroll_wait = 60;
                    }
                } else {
                    p->next_scroll_x += 2;
                    if (p->next_scroll_x >= max_scroll) {
                        p->next_scroll_x = max_scroll;
                        p->next_scroll_wait = 60;
                    }
                }
                pd->graphics->setClipRect(next_title_left, 24, title_max_w, FONT_HEIGHT);
                pd->graphics->drawText(p->next_title, next_len, kASCIIEncoding,
                                       next_right - next_w + p->next_scroll_x, y_prev_next);
                pd->graphics->clearClipRect();
            }
        }
    }

    /* ── Title (centered, first content row, with marquee) ────────── */
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
        const int title_y = 72;        /* right above seek bar at y=84 */

        if (text_w <= title_area_w) {
            /* Fits: center horizontally */
            int title_x = (400 - text_w) / 2;
            pd->graphics->drawText(name, name_len,
                                   kASCIIEncoding, title_x, title_y);
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

            pd->graphics->setClipRect(12, title_y, title_area_w, FONT_HEIGHT);
            pd->graphics->drawText(name, name_len, kASCIIEncoding,
                                   12 - p->title_scroll_x, title_y);
            pd->graphics->clearClipRect();
        }
    }

    /* ── Centered stack: progress bar + time + waveform ──────────── */
    /* Available zone: y=84 (below title at y=58+FONT_HEIGHT+15px margin) to y=216 (above controls) */
    {
        const int zone_top = 84, zone_bot = 216;
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

    /* ── Button bar (visual icons) ──────────────────────────────── */
    draw_button_bar(pd, p);
}

/* ══════════════════════════════════════════════════════════════════════
 * BUTTON BAR DRAWING
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_triangle_right(PlaydateAPI* pd, int cx, int cy, int size, int color) {
    /* Right-pointing triangle using lines */
    int x1 = cx - size/2, y1 = cy - size/2;
    int x2 = cx + size/2, y2 = cy;
    int x3 = cx - size/2, y3 = cy + size/2;
    pd->graphics->drawLine(x1, y1, x2, y2, 1, color);
    pd->graphics->drawLine(x2, y2, x3, y3, 1, color);
    pd->graphics->drawLine(x3, y3, x1, y1, 1, color);
}

static void draw_triangle_left(PlaydateAPI* pd, int cx, int cy, int size, int color) {
    /* Left-pointing triangle using lines */
    int x1 = cx + size/2, y1 = cy - size/2;
    int x2 = cx - size/2, y2 = cy;
    int x3 = cx + size/2, y3 = cy + size/2;
    pd->graphics->drawLine(x1, y1, x2, y2, 1, color);
    pd->graphics->drawLine(x2, y2, x3, y3, 1, color);
    pd->graphics->drawLine(x3, y3, x1, y1, 1, color);
}

static void draw_button_bar(PlaydateAPI* pd, VGMPlayer* p) {
    const int bar_y = 216;
    const int bar_h = 24;
    const int btn_w = 50;
    const int btn_h = 20;
    const int spacing = 4;
    const int total_w = 7 * btn_w + 6 * spacing;
    const int start_x = (400 - total_w) / 2;

    /* Initialize button positions and selected button on first call */
    if (p->buttons[0].w == 0) {
        for (int i = 0; i < 7; i++) {
            p->buttons[i].x = start_x + i * (btn_w + spacing);
            p->buttons[i].y = bar_y + 2;
            p->buttons[i].w = btn_w;
            p->buttons[i].h = btn_h;
        }
        p->selected_button = 2;  /* Start with play/pause button selected */
    }

    /* Draw bar background */
    pd->graphics->fillRect(0, bar_y, 400, bar_h, kColorWhite);
    pd->graphics->drawLine(0, bar_y, 400, bar_y, 1, kColorBlack);

    int icon_size = 8;  /* size of drawn icons */
    int cx, cy;

    /* Button 0: Previous track (|◀◀) */
    cx = p->buttons[0].x + p->buttons[0].w / 2;
    cy = p->buttons[0].y + p->buttons[0].h / 2;
    if (p->selected_button == 0) {
        pd->graphics->fillRect(p->buttons[0].x, p->buttons[0].y, p->buttons[0].w, p->buttons[0].h, kColorBlack);
        pd->graphics->fillRect(cx - 9, cy - 5, 2, 10, kColorWhite);
        draw_triangle_left(pd, cx - 3, cy, icon_size, kColorWhite);
        draw_triangle_left(pd, cx + 4, cy, icon_size, kColorWhite);
    } else {
        pd->graphics->drawRect(p->buttons[0].x, p->buttons[0].y, p->buttons[0].w, p->buttons[0].h, kColorBlack);
        pd->graphics->fillRect(cx - 9, cy - 5, 2, 10, kColorBlack);
        draw_triangle_left(pd, cx - 3, cy, icon_size, kColorBlack);
        draw_triangle_left(pd, cx + 4, cy, icon_size, kColorBlack);
    }

    /* Button 1: Seek backward (◄◄) */
    cx = p->buttons[1].x + p->buttons[1].w / 2;
    cy = p->buttons[1].y + p->buttons[1].h / 2;
    if (p->selected_button == 1) {
        pd->graphics->fillRect(p->buttons[1].x, p->buttons[1].y, p->buttons[1].w, p->buttons[1].h, kColorBlack);
        draw_triangle_left(pd, cx - 3, cy, icon_size, kColorWhite);
        draw_triangle_left(pd, cx + 3, cy, icon_size, kColorWhite);
    } else {
        pd->graphics->drawRect(p->buttons[1].x, p->buttons[1].y, p->buttons[1].w, p->buttons[1].h, kColorBlack);
        draw_triangle_left(pd, cx - 3, cy, icon_size, kColorBlack);
        draw_triangle_left(pd, cx + 3, cy, icon_size, kColorBlack);
    }

    /* Button 2: Play/Pause */
    cx = p->buttons[2].x + p->buttons[2].w / 2;
    cy = p->buttons[2].y + p->buttons[2].h / 2;
    if (p->selected_button == 2) {
        pd->graphics->fillRect(p->buttons[2].x, p->buttons[2].y, p->buttons[2].w, p->buttons[2].h, kColorBlack);
        if (p->state == STATE_PLAYING) {
            /* Pause icon: two vertical bars (white) */
            pd->graphics->fillRect(cx - 4, cy - 4, 2, 8, kColorWhite);
            pd->graphics->fillRect(cx + 2, cy - 4, 2, 8, kColorWhite);
        } else {
            /* Play icon: right triangle (white) */
            draw_triangle_right(pd, cx, cy, icon_size, kColorWhite);
        }
    } else {
        pd->graphics->drawRect(p->buttons[2].x, p->buttons[2].y, p->buttons[2].w, p->buttons[2].h, kColorBlack);
        if (p->state == STATE_PLAYING) {
            /* Pause icon: two vertical bars (black) */
            pd->graphics->fillRect(cx - 4, cy - 4, 2, 8, kColorBlack);
            pd->graphics->fillRect(cx + 2, cy - 4, 2, 8, kColorBlack);
        } else {
            /* Play icon: right triangle (black) */
            draw_triangle_right(pd, cx, cy, icon_size, kColorBlack);
        }
    }

    /* Button 3: Seek forward (►►) */
    cx = p->buttons[3].x + p->buttons[3].w / 2;
    cy = p->buttons[3].y + p->buttons[3].h / 2;
    if (p->selected_button == 3) {
        pd->graphics->fillRect(p->buttons[3].x, p->buttons[3].y, p->buttons[3].w, p->buttons[3].h, kColorBlack);
        draw_triangle_right(pd, cx - 3, cy, icon_size, kColorWhite);
        draw_triangle_right(pd, cx + 3, cy, icon_size, kColorWhite);
    } else {
        pd->graphics->drawRect(p->buttons[3].x, p->buttons[3].y, p->buttons[3].w, p->buttons[3].h, kColorBlack);
        draw_triangle_right(pd, cx - 3, cy, icon_size, kColorBlack);
        draw_triangle_right(pd, cx + 3, cy, icon_size, kColorBlack);
    }

    /* Button 4: Next track (►►|) */
    cx = p->buttons[4].x + p->buttons[4].w / 2;
    cy = p->buttons[4].y + p->buttons[4].h / 2;
    if (p->selected_button == 4) {
        pd->graphics->fillRect(p->buttons[4].x, p->buttons[4].y, p->buttons[4].w, p->buttons[4].h, kColorBlack);
        draw_triangle_right(pd, cx - 4, cy, icon_size, kColorWhite);
        draw_triangle_right(pd, cx + 3, cy, icon_size, kColorWhite);
        pd->graphics->fillRect(cx + 7, cy - 5, 2, 10, kColorWhite);
    } else {
        pd->graphics->drawRect(p->buttons[4].x, p->buttons[4].y, p->buttons[4].w, p->buttons[4].h, kColorBlack);
        draw_triangle_right(pd, cx - 4, cy, icon_size, kColorBlack);
        draw_triangle_right(pd, cx + 3, cy, icon_size, kColorBlack);
        pd->graphics->fillRect(cx + 7, cy - 5, 2, 10, kColorBlack);
    }

    /* Button 5: Shuffle */
    cx = p->buttons[5].x + p->buttons[5].w / 2;
    cy = p->buttons[5].y + p->buttons[5].h / 2;
    if (p->shuffle_enabled || p->selected_button == 5) {
        pd->graphics->fillRect(p->buttons[5].x, p->buttons[5].y, p->buttons[5].w, p->buttons[5].h, kColorBlack);
        pd->graphics->setDrawMode(kDrawModeInverted);
        pd->graphics->drawText("S", 1, kASCIIEncoding, cx - 3, cy - 8);
        pd->graphics->setDrawMode(kDrawModeCopy);
    } else {
        pd->graphics->drawRect(p->buttons[5].x, p->buttons[5].y, p->buttons[5].w, p->buttons[5].h, kColorBlack);
        pd->graphics->drawText("S", 1, kASCIIEncoding, cx - 3, cy - 8);
    }



    /* Button 6: Repeat */
    cx = p->buttons[6].x + p->buttons[6].w / 2;
    cy = p->buttons[6].y + p->buttons[6].h / 2;
    if (p->repeat_mode > 0 || p->selected_button == 6) {
        pd->graphics->fillRect(p->buttons[6].x, p->buttons[6].y, p->buttons[6].w, p->buttons[6].h, kColorBlack);
        pd->graphics->setDrawMode(kDrawModeInverted);
        pd->graphics->drawText("R", 1, kASCIIEncoding, cx - 3, cy - 8);
        pd->graphics->setDrawMode(kDrawModeCopy);
    } else {
        pd->graphics->drawRect(p->buttons[6].x, p->buttons[6].y, p->buttons[6].w, p->buttons[6].h, kColorBlack);
        pd->graphics->drawText("R", 1, kASCIIEncoding, cx - 3, cy - 8);
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

/* ══════════════════════════════════════════════════════════════════════
 * PLAYBACK MODE HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

static void shuffle_files(VGMPlayer* p) {
    /* Only include actual audio files (not "../" or subdirectory entries) */
    p->shuffled_count = 0;
    for (int i = 0; i < p->browser_count; i++) {
        if (p->browser_is_dir[i] == 0)
            p->shuffled_indices[p->shuffled_count++] = i;
    }
    /* Fisher-Yates shuffle using rand() for randomness */
    for (int i = p->shuffled_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = p->shuffled_indices[i];
        p->shuffled_indices[i] = p->shuffled_indices[j];
        p->shuffled_indices[j] = tmp;
    }
}

static int get_next_song_index(VGMPlayer* p) {
    if (p->browser_count == 0) return -1;
    if (p->current_song_index < 0) {
        /* Find first playable file (skip any leading dir entries) */
        for (int i = 0; i < p->browser_count; i++)
            if (p->browser_is_dir[i] == 0) return i;
        return -1;
    }

    int next_idx = -1;

    if (p->shuffle_enabled) {
        /* Find current song in shuffled list */
        int pos = -1;
        for (int i = 0; i < p->shuffled_count; i++) {
            if (p->shuffled_indices[i] == p->current_song_index) {
                pos = i;
                break;
            }
        }
        if (pos >= 0 && pos < p->shuffled_count - 1) {
            next_idx = p->shuffled_indices[pos + 1];
        } else if (p->repeat_mode == 2) {
            /* Repeat all: reshuffle and start from beginning */
            shuffle_files(p);
            next_idx = (p->shuffled_count > 0) ? p->shuffled_indices[0] : -1;
        }
    } else {
        /* Step forward, skipping directory entries */
        next_idx = p->current_song_index + 1;
        while (next_idx < p->browser_count && p->browser_is_dir[next_idx] != 0)
            next_idx++;
        if (next_idx >= p->browser_count) {
            if (p->repeat_mode == 2) {
                /* Repeat all: wrap to first playable file */
                next_idx = 0;
                while (next_idx < p->browser_count && p->browser_is_dir[next_idx] != 0)
                    next_idx++;
                if (next_idx >= p->browser_count) next_idx = -1;
            } else {
                next_idx = -1;
            }
        }
    }

    return next_idx;
}

/* Read the embedded title for a file (SID: title/author, MOD/XM/IT/S3M: module
 * name). Falls back to the bare filename when no embedded title is found. */
static void title_for_file(const char* path, char* buf, int buf_size)
{
    VGMPlayer* p = &g_player;
    const char* ext = strrchr(path, '.');
    buf[0] = '\0';

    if (ext && strcasecmp(ext, ".sid") == 0) {
        /* Read title/author directly from the PSID/RSID header (fixed offsets)
         * WITHOUT initialising the cRSID C64 emulator — cRSID_init() returns a
         * pointer to a global C64 instance, so calling sid_player_open() here
         * would overwrite the emulator state of the currently-playing SID. */
        SDFile* f = p->pd->file->open(path, kFileRead | kFileReadData);
        if (f) {
            /* PSID/RSID header layout:
             *   0x00  magic ("PSID"/"RSID")  4 bytes
             *   0x16  title                  32 bytes (may not be NUL-terminated)
             *   0x36  author                 32 bytes */
            unsigned char hdr[0x56];
            if (p->pd->file->read(f, hdr, sizeof(hdr)) == (int)sizeof(hdr)) {
                char title[33];
                memcpy(title, hdr + 0x16, 32); title[32] = '\0';
                if (title[0] != '\0')
                    strncpy(buf, title, buf_size - 1);
            }
            p->pd->file->close(f);
        }
    } else if (ext && (strcasecmp(ext, ".mod") == 0 ||
                       strcasecmp(ext, ".xm")  == 0 ||
                       strcasecmp(ext, ".it")  == 0 ||
                       strcasecmp(ext, ".s3m") == 0)) {
        ModPlayer* mp = mod_player_open(p->pd, path, /*force_mono=*/0);
        if (mp) {
            const char* title = mod_player_title(mp);
            if (title[0] != '\0')
                strncpy(buf, title, buf_size - 1);
            mod_player_close(mp);
        }
    }
    /* VGM/other formats: no cheap metadata access — fall through to filename */

    /* Fallback to bare filename */
    if (buf[0] == '\0') {
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;
        strncpy(buf, name, buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

static void update_prev_next_display(VGMPlayer* p) {
    /* Get previous song index */
    int prev_idx = -1;
    if (p->shuffle_enabled) {
        for (int i = 0; i < p->shuffled_count; i++) {
            if (p->shuffled_indices[i] == p->current_song_index && i > 0) {
                prev_idx = p->shuffled_indices[i - 1];
                break;
            }
        }
    } else {
        int prev_cand = p->current_song_index - 1;
        while (prev_cand >= 0 && p->browser_is_dir[prev_cand] != 0)
            prev_cand--;
        if (prev_cand >= 0)
            prev_idx = prev_cand;
    }

    /* Get next song index */
    int next_idx = get_next_song_index(p);

    if (prev_idx >= 0 && prev_idx < p->browser_count) {
        title_for_file(p->browser_files[prev_idx], p->prev_title, sizeof(p->prev_title));
        p->prev_scroll_x = 0;
        p->prev_scroll_wait = 60;
    } else {
        p->prev_title[0] = '\0';
    }

    if (next_idx >= 0 && next_idx < p->browser_count) {
        title_for_file(p->browser_files[next_idx], p->next_title, sizeof(p->next_title));
        p->next_scroll_x = 0;
        p->next_scroll_wait = 60;
    } else {
        p->next_title[0] = '\0';
    }
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

/* Shared button handler for STATE_PLAYING and STATE_PAUSED */
static void handle_playback_input(VGMPlayer* p, PDButtons pushed)
{
    /* D-pad left/right → cycle through button bar */
    if (pushed & kButtonLeft) {
        p->selected_button = (p->selected_button - 1 + 7) % 7;
        p->needs_redraw = 1;
    }
    if (pushed & kButtonRight) {
        p->selected_button = (p->selected_button + 1) % 7;
        p->needs_redraw = 1;
    }
    if (pushed & kButtonA) {
        switch (p->selected_button) {
            case 0: { /* Previous track */
                if (p->browser_count > 0 && p->current_song_index >= 0) {
                    int prev_idx = -1;
                    if (p->shuffle_enabled) {
                        for (int i = 0; i < p->shuffled_count; i++) {
                            if (p->shuffled_indices[i] == p->current_song_index && i > 0) {
                                prev_idx = p->shuffled_indices[i - 1];
                                break;
                            }
                        }
                    } else {
                        int prev_cand = p->current_song_index - 1;
                        while (prev_cand >= 0 && p->browser_is_dir[prev_cand] != 0)
                            prev_cand--;
                        if (prev_cand >= 0)
                            prev_idx = prev_cand;
                    }
                    if (prev_idx >= 0 && prev_idx < p->browser_count) {
                        p->current_song_index = prev_idx;
                        if (player_open_file(p->browser_files[prev_idx])) {
                            update_prev_next_display(p);
                            player_play();
                            p->needs_redraw = 1;
                        }
                    }
                }
                break;
            }
            case 1:  /* Seek backward 5 sec */
                if (p->total_samples > 0)
                    player_seek(p->current_sample - p->sample_rate * 5);
                break;
            case 2:  /* Play/Pause toggle */
                if (p->state == STATE_PAUSED) player_play();
                else player_pause();
                break;
            case 3:  /* Seek forward 5 sec */
                if (p->total_samples > 0)
                    player_seek(p->current_sample + p->sample_rate * 5);
                break;
            case 4: { /* Next track */
                int next_idx = get_next_song_index(p);
                if (next_idx >= 0 && next_idx < p->browser_count) {
                    p->current_song_index = next_idx;
                    if (player_open_file(p->browser_files[next_idx])) {
                        update_prev_next_display(p);
                        player_play();
                        p->needs_redraw = 1;
                    }
                }
                break;
            }
            case 5:  /* Toggle shuffle */
                p->shuffle_enabled = !p->shuffle_enabled;
                if (p->shuffle_enabled && p->current_song_index >= 0)
                    shuffle_files(p);
                p->needs_redraw = 1;
                break;
            case 6:  /* Cycle repeat mode */
                p->repeat_mode = (p->repeat_mode + 1) % 3;
                p->needs_redraw = 1;
                break;
        }
    }
    if (pushed & kButtonB)
        player_stop();
}

static void handle_input(void)
{
    VGMPlayer* p = &g_player;
    PlaydateAPI* pd = p->pd;

    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);

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
                int sel = p->browser_selection;
                if (p->browser_is_dir[sel] != 0) {
                    /* Navigate into subdir or up to parent */
                    int was_subdir = (p->browser_is_dir[sel] == 1);
                    browser_scan(p->browser_files[sel]);
                    /* When entering a subdir (not going up), skip the "../"
                     * entry so the cursor lands on the first real file. */
                    if (was_subdir && p->browser_count > 1)
                        p->browser_selection = 1;
                    p->needs_redraw = 1;
                } else {
                    /* Play the selected file */
                    p->current_song_index = sel;
                    if (p->shuffle_enabled)
                        shuffle_files(p);
                    if (player_open_file(p->browser_files[sel])) {
                        update_prev_next_display(p);
                        player_play();
                    }
                }
            }
        }
        if (pushed & kButtonB) {
            /* Go up one directory level */
            if (strcmp(p->current_dir, "vgm") != 0) {
                char parent_path[MAX_PATH];
                strncpy(parent_path, p->current_dir, sizeof(parent_path) - 1);
                char* last_slash = strrchr(parent_path, '/');
                if (last_slash) *last_slash = '\0';
                else strncpy(parent_path, "vgm", sizeof(parent_path) - 1);
                browser_scan(parent_path);
                p->needs_redraw = 1;
            }
        }
        break;

    case STATE_PLAYING:
    case STATE_PAUSED:
        handle_playback_input(p, pushed);
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

static void menu_mono_toggle(void* userdata) {
    VGMPlayer* p = (VGMPlayer*)userdata;
    p->force_mono = p->pd->system->getMenuItemValue(p->menu_mono);

    /* Reopen the current file to apply the new channel mode */
    if ((p->mod || p->vgm) && p->current_file[0] != '\0') {
        int was_playing = (p->state == STATE_PLAYING);
        int32_t pos = p->current_sample;

        /* Remove old audio source */
        if (p->audio_source) {
            p->pd->sound->removeSource(p->audio_source);
            p->audio_source = NULL;
        }

        if (player_open_file(p->current_file)) {
            if (was_playing) {
                player_play();
                if (pos > 0) player_seek(pos);
            }
        }
    }
    p->needs_redraw = 1;
}

static void menu_repeat_cycle(void* userdata) {
    VGMPlayer* p = (VGMPlayer*)userdata;
    p->repeat_mode = p->pd->system->getMenuItemValue(p->menu_repeat);
    p->needs_redraw = 1;
}

static void menu_shuffle_toggle(void* userdata) {
    VGMPlayer* p = (VGMPlayer*)userdata;
    int shuffle_mode = p->pd->system->getMenuItemValue(p->menu_shuffle);
    /* 0 = Off, 1 = Directory, 2 = All */
    p->shuffle_enabled = (shuffle_mode > 0) ? 1 : 0;
    if (p->shuffle_enabled && p->current_song_index >= 0) {
        shuffle_files(p);
    }
    p->needs_redraw = 1;
}

static void player_init(PlaydateAPI* pd)
{
    memset(&g_player, 0, sizeof(VGMPlayer));
    g_player.pd     = pd;
    g_player.state  = STATE_BROWSER;
    g_player.vis_enabled = 1;
    g_player.force_mono = 1;  /* default: mono for better device performance */
    g_player.needs_redraw = 1;

    /* Initialise Playdate filesystem adapter for vgmstream */
    vgm_pd_streamfile_set_api(pd);

    /* Menu items will be created when playback starts */

    /* Scan for audio files */
    browser_scan("vgm");
}

static int update(void* userdata)
{
    (void)userdata;
    VGMPlayer* p = &g_player;

    handle_input();

    int will_draw = p->needs_redraw;

    /* Pre-fill audio ring buffer from the main thread so the audio
     * callback never has to decode (which would cause lag/stuttering).
     * Use a larger budget on non-draw frames where we have more time. */
    float budget = will_draw ? DECODE_BUDGET_DRAW_S : DECODE_BUDGET_NODRAW_S;
    player_prefill_audio(budget);

    /* Only redraw when needed (saves CPU) */
    if (will_draw) {
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
        if (g_player.vgm || g_player.sid || g_player.mod)
            player_play();
        break;

    default:
        break;
    }

    return 0;
}
