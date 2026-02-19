/*
 * sid_player_crsid.c — Playdate SID player using cRSID by Hermit (Mihaly Horvath)
 *
 * cRSID is a lightweight integer-only C64/SID emulator featuring:
 *   - Cycle-accurate 6510 CPU emulation
 *   - CIA / VIC timing (IRQ, NMI, hard-restart, delay bugs)
 *   - PSID v1–v4 and RSID file format support (up to 3 SID chips)
 *   - Chip-model-accurate filter tables for both 6581 and 8580
 *   - ADSR with proper exponential curves via lookup tables
 *   - All waveforms including combined-waveform lookup tables
 *
 * License: WTF — do what you want with the code; credit Hermit as original author.
 * Repository: https://github.com/r-moeritz/crsid-by-hermit
 *
 * Integration notes
 * -----------------
 * cRSID ships as a unity build.  We include libcRSID.c here so that all of
 * C64/C64.c, C64/CPU.c, C64/SID.c … and the static-inline cRSID_generateSample()
 * from host/audio.c are compiled in the same translation unit.
 * CMakeLists.txt lists only *this* file — it must NOT also list any crsid/*.c.
 *
 * Do NOT define CRSID_PLATFORM_PC — it pulls in SDL and stdio file I/O.
 */

#include "libcRSID.c"   /* unity build — includes C64/ and host/ internals */

#include "sid_player.h"
#include <stdlib.h>
#include <string.h>

struct SidPlayer {
    PlaydateAPI*        pd;
    cRSID_C64instance*  c64;
    unsigned char*      filedata;  /* must outlive playback — C64->SIDheader points into it */
    char                title[33];
    char                author[33];
};

SidPlayer* sid_player_open(PlaydateAPI* pd, const char* path)
{
    /* ── Read the SID file from the Playdate filesystem ──────────────── */
    SDFile* f = pd->file->open(path, kFileRead | kFileReadData);
    if (!f) return NULL;

    pd->file->seek(f, 0, SEEK_END);
    int filesize = pd->file->tell(f);
    pd->file->seek(f, 0, SEEK_SET);

    if (filesize <= 0 || filesize > 100000) { pd->file->close(f); return NULL; }

    unsigned char* filedata = (unsigned char*)malloc(filesize);
    if (!filedata) { pd->file->close(f); return NULL; }

    if (pd->file->read(f, filedata, filesize) != filesize) {
        free(filedata); pd->file->close(f); return NULL;
    }
    pd->file->close(f);

    /* ── Init cRSID ───────────────────────────────────────────────────
     * In non-PC mode cRSID_init() just calls cRSID_createC64() and
     * returns a pointer to the global cRSID_C64 instance (no SDL).
     * buflen=0 is fine for embedded use; the argument only suppresses a
     * compiler warning in the non-PC code path.                        */
    cRSID_C64instance* c64 = cRSID_init(22050, 0);
    if (!c64) { free(filedata); return NULL; }

    /* ── Process the PSID/RSID file ──────────────────────────────────
     * Music data is copied into c64->RAMbank[].  The SID header fields
     * (Title, Author, subtune count, etc.) are read directly from the
     * filedata buffer via c64->SIDheader, so filedata must stay alive. */
    cRSID_SIDheader* header = cRSID_processSIDfile(c64, filedata, filesize);
    if (!header) { free(filedata); return NULL; }

    /* ── Init the default sub-song (0 → cRSID selects the default) ── */
    cRSID_initSIDtune(c64, header, 0);

    /* ── Wrap in our player struct ─────────────────────────────────── */
    SidPlayer* sp = (SidPlayer*)malloc(sizeof(SidPlayer));
    if (!sp) { free(filedata); return NULL; }

    sp->pd       = pd;
    sp->c64      = c64;
    sp->filedata = filedata;

    /* Title and Author are 32-char arrays in the PSID header; they are
     * NOT NUL-terminated in the spec, so we copy and add the terminator. */
    memcpy(sp->title,  header->Title,  32); sp->title[32]  = '\0';
    memcpy(sp->author, header->Author, 32); sp->author[32] = '\0';

    return sp;
}

void sid_player_close(SidPlayer* sp)
{
    if (!sp) return;
    free(sp->filedata);
    free(sp);
}

int sid_player_fill(SidPlayer* sp, int16_t* buf, int n_samples)
{
    /* SID hardware is mono — output mono samples directly.
     * main.c's audio callback duplicates mono to both L/R channels. */
    for (int i = 0; i < n_samples; i++) {
        buf[i] = cRSID_generateSample(sp->c64);
    }
    return n_samples;
}

int         sid_player_channels    (SidPlayer* sp) { (void)sp; return 1;     }
int         sid_player_sample_rate (SidPlayer* sp) { (void)sp; return 22050; }
int32_t     sid_player_total_samples(SidPlayer* sp){ (void)sp; return 0;     }
const char* sid_player_title       (SidPlayer* sp) { return sp->title;       }
const char* sid_player_author      (SidPlayer* sp) { return sp->author;      }
