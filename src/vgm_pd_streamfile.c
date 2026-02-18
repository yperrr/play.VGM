/**
 * vgm_pd_streamfile.c — Playdate filesystem adapter for vgmstream
 *
 * Implements libstreamfile_t by populating the struct's function pointers
 * directly (there is no libstreamfile_open_from_callbacks() factory).
 *
 * The result is wrapped with libstreamfile_open_buffered() because
 * vgmstream seeks non-linearly and the cache dramatically reduces
 * individual pd->file->seek / pd->file->read calls.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_api.h"
#include "libvgmstream.h"
#include "libvgmstream_streamfile.h"

/* ── We need the Playdate API pointer globally for I/O ──────────── */
static PlaydateAPI* s_pd = NULL;

void vgm_pd_streamfile_set_api(PlaydateAPI* pd)
{
    s_pd = pd;
}

/* ── Internal state per streamfile instance ─────────────────────── */
typedef struct {
    SDFile*     file;
    char        name[256];
    int64_t     file_size;
} pd_sf_data;

/* ── libstreamfile_t callbacks ───────────────────────────────────── */

static int pd_sf_read(void* user_data, uint8_t* dst,
                      int64_t offset, int length)
{
    pd_sf_data* data = (pd_sf_data*)user_data;
    if (!data->file || !s_pd) return 0;

    s_pd->file->seek(data->file, (int)offset, SEEK_SET);
    int n = s_pd->file->read(data->file, dst, (unsigned int)length);
    return (n > 0) ? n : 0;
}

static int64_t pd_sf_get_size(void* user_data)
{
    pd_sf_data* data = (pd_sf_data*)user_data;
    return data->file_size;
}

static const char* pd_sf_get_name(void* user_data)
{
    pd_sf_data* data = (pd_sf_data*)user_data;
    return data->name;
}

/* Forward-declare so pd_sf_open can reference it */
static libstreamfile_t* pd_make_streamfile(const char* path);

static struct libstreamfile_t* pd_sf_open(void* user_data,
                                           const char* filename)
{
    (void)user_data;
    return pd_make_streamfile(filename);
}

static void pd_sf_close(struct libstreamfile_t* libsf)
{
    if (!libsf) return;
    pd_sf_data* data = (pd_sf_data*)libsf->user_data;
    if (data) {
        if (data->file && s_pd)
            s_pd->file->close(data->file);
        free(data);
    }
    free(libsf);
}

/* ── Build a raw (unbuffered) libstreamfile_t ────────────────────── */
static libstreamfile_t* pd_make_streamfile(const char* path)
{
    if (!s_pd || !path) return NULL;

    SDFile* file = s_pd->file->open(path, kFileRead | kFileReadData);
    if (!file) {
        s_pd->system->logToConsole("VGM SF: cannot open %s", path);
        return NULL;
    }

    /* Get file size */
    s_pd->file->seek(file, 0, SEEK_END);
    int sz = s_pd->file->tell(file);
    s_pd->file->seek(file, 0, SEEK_SET);

    pd_sf_data* data = (pd_sf_data*)calloc(1, sizeof(pd_sf_data));
    if (!data) {
        s_pd->file->close(file);
        return NULL;
    }
    data->file      = file;
    data->file_size = (int64_t)sz;
    strncpy(data->name, path, sizeof(data->name) - 1);

    libstreamfile_t* sf = (libstreamfile_t*)calloc(1, sizeof(libstreamfile_t));
    if (!sf) {
        s_pd->file->close(file);
        free(data);
        return NULL;
    }

    sf->user_data = data;
    sf->read      = pd_sf_read;
    sf->get_size  = pd_sf_get_size;
    sf->get_name  = pd_sf_get_name;
    sf->open      = pd_sf_open;
    sf->close     = pd_sf_close;

    return sf;
}

/* ══════════════════════════════════════════════════════════════════
 * PUBLIC: Create a buffered vgmstream streamfile from a
 *         Playdate filesystem path
 * ══════════════════════════════════════════════════════════════════ */

libstreamfile_t* vgm_pd_open_streamfile(const char* path)
{
    /* Note: do NOT wrap with libstreamfile_open_buffered() — its cache_read
     * passes its own priv as user_data to the inner read callback, which
     * crashes because our pd_sf_read expects pd_sf_data*.  The raw
     * streamfile works fine; Playdate's file API handles seeking natively. */
    return pd_make_streamfile(path);
}
