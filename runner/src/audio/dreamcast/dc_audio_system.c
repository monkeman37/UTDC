/* AICA audio backend for Butterscotch on the Sega Dreamcast.
 *
 *  Sound effects : mono 4-bit ADPCM, uploaded to the 2 MB sound RAM on first
 *                  use and kept in an LRU cache.
 *  Music streams : ADPCM streamed from disc through KOS snd_stream; a
 *                  background thread polls the streams so disc reads never
 *                  stall the game loop.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <dc/sound/aica_comm.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "dc_audio_system.h"
#include "../../../../common/utdc_formats.h"
#include "utils.h"
#include "log.h"
#include "stb_ds.h"

#define MAX_INSTANCES 48
#define MAX_STREAM_SLOTS 64
#define MAX_ACTIVE_STREAMS 4       /* KOS supports at most 4 */
/* Streams are decoded from ADPCM to 16-bit PCM on the SH-4, so the decoder state is always known
   (exact pause/resume, pitch changes, loops). AICA side: 64 KB per channel = 0.74 s at 44.1 kHz. */
#define STREAM_BUF_PER_CHANNEL SND_STREAM_BUFFER_MAX
/* ADPCM read-ahead in main RAM, refilled from disc in the stream thread (~1.1 s of stereo) */
#define STREAM_PREFETCH (48 * 1024)
#define STREAM_READ_CHUNK (16 * 1024)
#define STREAM_DECODE_FRAMES 1024
#define SFX_CACHE_BUDGET (1100 * 1024)
/* AICA channels reserved for sound effects (streams take 2 each from the rest) */
#define SFX_CHANNELS 40
#define STREAM_CHECKPOINTS 32

typedef struct {
    sfxhnd_t hnd;
    uint32_t lastUse;
} SfxCacheEntry;

typedef struct DcAudioSystem DcAudioSystem;

typedef struct {
    bool active;
    int32_t id;
    int32_t soundIndex;      /* SOND index or stream index */
    int32_t entry;
    uint8_t kind;
    bool loop;
    bool paused;
    float baseVolume;        /* SOND volume */
    float gain, startGain, targetGain, fadeTime, fadeTotal;
    float pitch;
    /* sfx */
    int chn;
    uint64_t startMs;
    uint64_t pausedAtMs;
    /* stream */
    snd_stream_hnd_t stream;
    file_t file;
    uint32_t pos;            /* ADPCM bytes read from disc into buf */
    bool finished;           /* reached the end (non-looping) */
    uint64_t drainUntilMs;   /* keep "playing" until the buffered tail has been heard */
    uint8_t* buf;            /* ADPCM read-ahead */
    uint32_t bufRd, bufWr;   /* valid bytes are buf[bufRd..bufWr) */
    uint32_t decPos;         /* ADPCM byte offset of the next byte to decode */
    int16_t hist[2], step[2];/* ADPCM decoder state per channel */
    uint32_t decSample;      /* song sample index of the next decoded sample */
    struct { uint32_t pos, sample; int16_t hist[2], step[2]; } cp[STREAM_CHECKPOINTS];
    int cpCount, cpHead;
    uint32_t playStartSample;/* song sample at which the current AICA playback started */
    uint64_t playStartMs;
    bool started;            /* AICA stream running */
    bool startPending;       /* start requested; the stream thread starts it once data is buffered */
    volatile bool ioBusy;    /* the stream thread is reading this stream's data (lock released) */
    int aicaCh[2];           /* the AICA channels KOS gave this stream (for live pitch changes) */
    DcAudioSystem* owner;
} DcInstance;

typedef struct {
    bool used;
    int32_t entry;
} StreamSlot;

struct DcAudioSystem {
    AudioSystem base;
    char path[128];
    file_t file;
    UtdcAudHeader hdr;
    int32_t* soundToEntry;
    UtdcAudEntry* entries;
    char* names;
    SfxCacheEntry* sfx;
    uint32_t sfxBytes;
    uint32_t useCounter;
    DcInstance inst[MAX_INSTANCES];
    int32_t nextId;
    StreamSlot streams[MAX_STREAM_SLOTS];
    float masterGain;
    bool allPaused;
    int sfxChan[64];      /* reserved AICA channel numbers */
    int sfxChanCount;
    int8_t sfxChanOwner[64]; /* instance index or -1 */
    mutex_t lock;
    kthread_t* thread;
    volatile bool quit;
};

static DcAudioSystem* g_audio = nullptr;

/* ----------------------------------------------------------------------- */

static void makeKey(const char* file, char* out, size_t outLen) {
    const char* base = file;
    for (const char* c = file; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    char tmp[128]; size_t i = 0;
    for (; base[i] && i < sizeof(tmp) - 1; i++) tmp[i] = (char) tolower((unsigned char) base[i]);
    tmp[i] = 0;
    char* dot = strrchr(tmp, '.');
    if (dot) *dot = 0;
    const char* k = tmp;
    if (!strncmp(k, "mus_", 4)) k += 4;
    snprintf(out, outLen, "%s", k);
}

static int32_t findEntryByKey(DcAudioSystem* a, const char* key) {
    for (uint32_t i = 0; i < a->hdr.entryCount; i++) {
        UtdcAudEntry* e = &a->entries[i];
        if (e->nameOffset != 0xFFFFFFFFu && e->nameOffset < a->hdr.nameBytes && !strcmp(a->names + e->nameOffset, key)) return (int32_t) i;
    }
    return -1;
}

static uint64_t nowMs(void) { return timer_ms_gettime64(); }

static int volToAica(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (int) (v * 255.0f + 0.5f);
}

static float effectiveGain(DcAudioSystem* a, DcInstance* in) {
    float group = in->soundIndex >= 0 && in->soundIndex < DC_AUDIO_STREAM_INDEX_BASE ? AudioSystem_soundGroupGain(&a->base, in->soundIndex) : 1.0f;
    return in->gain * in->baseVolume * group * a->masterGain;
}

static void aicaChannelUpdate(int chn, uint32_t what, int vol, uint32_t freq) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = (uint32_t) chn;
    chan->cmd = AICA_CH_CMD_UPDATE | what;
    chan->vol = (uint32_t) vol;
    chan->freq = freq;
    snd_sh4_to_aica(tmp, cmd->size);
}

/* ----------------------------------------------------------------------- */
/* Sound effects                                                           */
/* ----------------------------------------------------------------------- */

static bool sfxInUse(DcAudioSystem* a, int32_t entry) {
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (a->inst[i].active && a->inst[i].entry == entry && a->inst[i].kind == UTDC_AUDKIND_SFX) return true;
    return false;
}

static bool sfxEvictOne(DcAudioSystem* a) {
    int32_t victim = -1; uint32_t oldest = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < a->hdr.entryCount; i++) {
        if (a->sfx[i].hnd == SFXHND_INVALID) continue;
        if (sfxInUse(a, (int32_t) i)) continue;
        if (a->sfx[i].lastUse < oldest) { oldest = a->sfx[i].lastUse; victim = (int32_t) i; }
    }
    if (victim < 0) return false;
    snd_sfx_unload(a->sfx[victim].hnd);
    a->sfx[victim].hnd = SFXHND_INVALID;
    a->sfxBytes -= a->entries[victim].size;
    return true;
}

static sfxhnd_t sfxEnsure(DcAudioSystem* a, int32_t entry) {
    SfxCacheEntry* c = &a->sfx[entry];
    c->lastUse = ++a->useCounter;
    if (c->hnd != SFXHND_INVALID) return c->hnd;
    UtdcAudEntry* e = &a->entries[entry];
    while (a->sfxBytes + e->size > SFX_CACHE_BUDGET) if (!sfxEvictOne(a)) break;
    uint8_t* buf = (uint8_t*) memalign(32, (e->size + 31) & ~31u);
    if (!buf) return SFXHND_INVALID;
    mutex_lock(&a->lock);
    fs_seek(a->file, e->offset, SEEK_SET);
    ssize_t got = fs_read(a->file, buf, e->size);
    mutex_unlock(&a->lock);
    if (got != (ssize_t) e->size) { free(buf); return SFXHND_INVALID; }
    c->hnd = snd_sfx_load_raw_buf((char*) buf, e->size, e->rate, 4, e->channels);
    free(buf);
    if (c->hnd == SFXHND_INVALID) {
        /* sound RAM full: drop everything not playing and retry once */
        while (sfxEvictOne(a)) {}
        return SFXHND_INVALID;
    }
    a->sfxBytes += e->size;
    return c->hnd;
}

/* ----------------------------------------------------------------------- */
/* Streams                                                                 */
/* ----------------------------------------------------------------------- */

static DcInstance* streamOwner(snd_stream_hnd_t hnd) {
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (g_audio->inst[i].active && g_audio->inst[i].kind == UTDC_AUDKIND_STREAM && g_audio->inst[i].stream == hnd) return &g_audio->inst[i];
    return nullptr;
}

/* AICA 4-bit ADPCM decoder (inverse of the builder's encoder, see builder/src/audio.c) */
static inline int16_t adpcmStep(uint8_t nib, int16_t* hist, int16_t* step) {
    static const int stepTable[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    int d = nib & 7;
    int diff = ((1 + (d << 1)) * *step) >> 3;
    if (diff > 32767) diff = 32767;
    int v = (nib & 8) ? *hist - diff : *hist + diff;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    int ns = (stepTable[d] * *step) >> 8;
    if (ns < 127) ns = 127;
    if (ns > 24576) ns = 24576;
    *step = (int16_t) ns;
    *hist = (int16_t) v;
    return (int16_t) v;
}

static void streamResetDecoder(DcInstance* in) {
    in->hist[0] = in->hist[1] = 0;
    in->step[0] = in->step[1] = 127;
}

/* bytes of ADPCM per stereo/mono frame pair: mono packs 2 frames per byte, stereo 1 frame per byte */
static inline uint32_t framesToAdpcmBytes(int channels, uint32_t frames) { return channels == 2 ? frames : frames / 2; }

/* Tops up the ADPCM read-ahead from disc. Called from the stream thread. */
static void streamPrefetch(DcAudioSystem* a, DcInstance* in) {
    UtdcAudEntry* e = &a->entries[in->entry];
    if (in->bufRd > 0 && in->bufRd == in->bufWr) in->bufRd = in->bufWr = 0;
    if (STREAM_PREFETCH - (in->bufWr - in->bufRd) < STREAM_READ_CHUNK) return;
    if (in->bufRd > 0) { memmove(in->buf, in->buf + in->bufRd, in->bufWr - in->bufRd); in->bufWr -= in->bufRd; in->bufRd = 0; }
    while (STREAM_PREFETCH - in->bufWr >= STREAM_READ_CHUNK) {
        if (in->pos >= e->size) {
            if (!in->loop) return;
            in->pos = 0;          /* wrap: the decoder resets when it reaches this point */
        }
        uint32_t n = STREAM_READ_CHUNK;
        if (n > e->size - in->pos) n = e->size - in->pos;
        fs_seek(in->file, e->offset + in->pos, SEEK_SET);
        ssize_t got = fs_read(in->file, in->buf + in->bufWr, n);
        if (got <= 0) return;
        in->bufWr += (uint32_t) got;
        in->pos += (uint32_t) got;
        if (n < STREAM_READ_CHUNK) return; /* reached end of file; continue next time */
    }
}

/* Decodes up to `frames` frames into L/R (R unused for mono). Returns frames decoded. */
static uint32_t streamDecode(DcAudioSystem* a, DcInstance* in, int16_t* L, int16_t* R, uint32_t frames) {
    UtdcAudEntry* e = &a->entries[in->entry];
    uint32_t done = 0;
    while (done < frames) {
        if (in->decPos >= e->size) {
            if (!in->loop) { in->finished = true; break; }
            in->decPos = 0; in->decSample = 0;
            streamResetDecoder(in);
        }
        if (in->bufRd >= in->bufWr) break; /* read-ahead empty (disc slow) */
        if (e->channels == 2) {
            uint32_t avail = in->bufWr - in->bufRd, left = e->size - in->decPos;
            uint32_t n = frames - done;
            if (n > avail) n = avail;
            if (n > left) n = left;
            const uint8_t* src = in->buf + in->bufRd;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t b = src[i];
                L[done + i] = adpcmStep(b >> 4, &in->hist[0], &in->step[0]);
                R[done + i] = adpcmStep(b & 15, &in->hist[1], &in->step[1]);
            }
            in->bufRd += n; in->decPos += n; done += n; in->decSample += n;
        } else {
            uint32_t avail = in->bufWr - in->bufRd, left = e->size - in->decPos;
            uint32_t nb = (frames - done) / 2;
            if (nb > avail) nb = avail;
            if (nb > left) nb = left;
            if (nb == 0) break;
            const uint8_t* src = in->buf + in->bufRd;
            for (uint32_t i = 0; i < nb; i++) {
                uint8_t b = src[i];
                L[done + 2 * i] = adpcmStep(b & 15, &in->hist[0], &in->step[0]);
                L[done + 2 * i + 1] = adpcmStep(b >> 4, &in->hist[0], &in->step[0]);
            }
            in->bufRd += nb; in->decPos += nb; done += nb * 2; in->decSample += nb * 2;
        }
    }
    return done;
}

static void streamCheckpoint(DcInstance* in) {
    int k = in->cpHead;
    in->cp[k].pos = in->decPos; in->cp[k].sample = in->decSample;
    in->cp[k].hist[0] = in->hist[0]; in->cp[k].hist[1] = in->hist[1];
    in->cp[k].step[0] = in->step[0]; in->cp[k].step[1] = in->step[1];
    in->cpHead = (k + 1) % STREAM_CHECKPOINTS;
    if (in->cpCount < STREAM_CHECKPOINTS) in->cpCount++;
}

/* Stream callback (stream thread, inside snd_stream_poll, a->lock held). Decodes into one shared
   buffer of interleaved 16-bit PCM; KOS splits the channels into sound RAM. */
static int16_t g_pcm[SND_STREAM_BUFFER_MAX / 2 * 2] __attribute__((aligned(32))); /* 64 KB */
static void* streamCallback(snd_stream_hnd_t hnd, int req, int* recv) {
    static int16_t L[STREAM_DECODE_FRAMES], R[STREAM_DECODE_FRAMES];
    DcInstance* in = streamOwner(hnd);
    *recv = 0;
    if (!in) return nullptr;
    UtdcAudEntry* e = &g_audio->entries[in->entry];
    int ch = e->channels == 2 ? 2 : 1;
    if (req > (int) sizeof g_pcm) req = (int) sizeof g_pcm;
    uint32_t framesReq = (uint32_t) req / (uint32_t) ch / 2u;
    streamCheckpoint(in);
    uint32_t written = 0;
    while (written < framesReq) {
        uint32_t n = framesReq - written;
        if (n > STREAM_DECODE_FRAMES) n = STREAM_DECODE_FRAMES;
        uint32_t got = (in->finished || in->paused) ? 0 : streamDecode(g_audio, in, L, R, n);
        if (got < n) {
            /* end of a one-shot stream (or the disc fell behind): pad with silence */
            memset(L + got, 0, (n - got) * 2);
            memset(R + got, 0, (n - got) * 2);
            if (in->finished && !in->drainUntilMs) {
                uint32_t bufFrames = STREAM_BUF_PER_CHANNEL / 2;
                in->drainUntilMs = timer_ms_gettime64() + (uint64_t) bufFrames * 1000u / (uint64_t) (e->rate * (in->pitch > 0 ? in->pitch : 1.0f)) + 50;
            }
        }
        if (ch == 2) {
            int16_t* o = g_pcm + written * 2;
            for (uint32_t i = 0; i < n; i++) { o[2 * i] = L[i]; o[2 * i + 1] = R[i]; }
        } else {
            memcpy(g_pcm + written, L, n * 2);
        }
        written += n;
    }
    *recv = (int) (written * 2u * (uint32_t) ch);
    return g_pcm;
}

static void waitIo(DcAudioSystem* a, DcInstance* in);
static void streamStartNow(DcAudioSystem* a, DcInstance* in);

/* True if the read-ahead has room for another chunk and there is more to read. */
static bool streamWantsData(DcAudioSystem* a, DcInstance* in) {
    UtdcAudEntry* e = &a->entries[in->entry];
    if (in->pos >= e->size && !in->loop) return false;
    return STREAM_PREFETCH - (in->bufWr - in->bufRd) >= STREAM_READ_CHUNK;
}

/* Disc reads happen here with a->lock released (the instance is marked ioBusy, which stop /
   seek / pause wait for), so the game thread never waits for the GD-ROM when it changes a
   volume or a pitch. snd_stream_poll (which runs the decode callback) is called with the lock. */
static void* streamThread(void* p) {
    DcAudioSystem* a = (DcAudioSystem*) p;
    while (!a->quit) {
        for (int i = 0; i < MAX_INSTANCES; i++) {
            mutex_lock(&a->lock);
            DcInstance* in = &a->inst[i];
            bool live = in->active && in->kind == UTDC_AUDKIND_STREAM && in->stream != SND_STREAM_INVALID && in->buf;
            if (live && (in->started || (in->startPending && !in->paused)) && streamWantsData(a, in)) {
                in->ioBusy = true;
                mutex_unlock(&a->lock);
                streamPrefetch(a, in);
                mutex_lock(&a->lock);
                in->ioBusy = false;
            }
            live = in->active && in->kind == UTDC_AUDKIND_STREAM && in->stream != SND_STREAM_INVALID && in->buf;
            if (live && in->startPending && !in->paused && !in->started) streamStartNow(a, in);
            if (live && in->started) {
                int pr = snd_stream_poll(in->stream);
                static uint64_t lastLog;
                if (getenv("BS_AUDIO_TRACE") && timer_ms_gettime64() - lastLog > 500) {
                    lastLog = timer_ms_gettime64();
                    logInfo("DC audio: poll -> %d (decSample %u, decPos %u/%u, rd %u wr %u pos %u)\n", pr, in->decSample, in->decPos, a->entries[in->entry].size, in->bufRd, in->bufWr, in->pos);
                }
            }
            mutex_unlock(&a->lock);
        }
        thd_sleep(10);
    }
    return nullptr;
}

/* Song sample the listener is hearing now (estimated from the time since the AICA started) */
static uint32_t streamPlayingSample(DcAudioSystem* a, DcInstance* in) {
    UtdcAudEntry* e = &a->entries[in->entry];
    if (!in->started) return in->playStartSample;
    uint64_t ms = timer_ms_gettime64() - in->playStartMs;
    uint64_t s = (uint64_t) in->playStartSample + (uint64_t) ((double) ms * (double) e->rate * (double) in->pitch / 1000.0);
    if (s > in->decSample && in->decSample >= in->playStartSample) s = in->decSample;
    return (uint32_t) (s % (e->samples ? e->samples : 1));
}

/* Repositions the decoder at song sample `target`: from the nearest checkpoint at or before it
   (or the start of the song), then decodes forward. Discards the read-ahead. a->lock held. */
static void streamSeekSample(DcAudioSystem* a, DcInstance* in, uint32_t target) {
    UtdcAudEntry* e = &a->entries[in->entry];
    waitIo(a, in);
    int best = -1;
    for (int i = 0; i < in->cpCount; i++)
        if (in->cp[i].sample <= target && (best < 0 || in->cp[i].sample > in->cp[best].sample)) best = i;
    if (best >= 0) {
        in->decPos = in->cp[best].pos; in->decSample = in->cp[best].sample;
        in->hist[0] = in->cp[best].hist[0]; in->hist[1] = in->cp[best].hist[1];
        in->step[0] = in->cp[best].step[0]; in->step[1] = in->cp[best].step[1];
    } else {
        in->decPos = 0; in->decSample = 0; streamResetDecoder(in);
    }
    in->pos = in->decPos; in->bufRd = in->bufWr = 0;
    in->finished = false; in->drainUntilMs = 0;
    static int16_t scratchL[STREAM_DECODE_FRAMES], scratchR[STREAM_DECODE_FRAMES];
    while (in->decSample < target) {
        streamPrefetch(a, in);
        uint32_t n = target - in->decSample;
        if (n > STREAM_DECODE_FRAMES) n = STREAM_DECODE_FRAMES;
        if (e->channels != 2) n &= ~1u;
        if (n == 0) break;
        if (streamDecode(a, in, scratchL, scratchR, n) == 0) break;
    }
    in->cpCount = 0; in->cpHead = 0;
}

/* Waits until the stream thread has finished reading this stream's data. a->lock held (it is
   released while waiting so the thread can finish). Only operations that touch the read-ahead
   need this (stop, seek, pause), and only right after a start does it ever wait. */
static void waitIo(DcAudioSystem* a, DcInstance* in) {
    while (in->ioBusy) { mutex_unlock(&a->lock); thd_pass(); mutex_lock(&a->lock); }
}

/* Requests AICA playback from the decoder's current position. a->lock held.
   The disc read and the AICA start happen on the stream thread: a start used to read 48 KB from
   the disc and prefill the AICA buffers on the game thread, a 20-40 ms hitch at every music
   change (far more on a real GD-ROM). */
static void streamStartAica(DcAudioSystem* a, DcInstance* in) {
    (void) a;
    in->playStartSample = in->decSample;
    in->startPending = true;
}

/* Stream thread, a->lock held, data buffered: starts the AICA side. */
static void streamStartNow(DcAudioSystem* a, DcInstance* in) {
    UtdcAudEntry* e = &a->entries[in->entry];
    uint32_t freq = (uint32_t) ((float) e->rate * in->pitch);
    if (freq < 1000) freq = 1000;
    if (freq > 88200) freq = 88200;
    in->playStartSample = in->decSample;
    snd_stream_start(in->stream, freq, e->channels == 2);
    in->playStartMs = timer_ms_gettime64();
    in->started = true;
    in->startPending = false;
    snd_stream_volume(in->stream, volToAica(effectiveGain(a, in)));
}

/* Stops AICA playback, rewinding the decoder to what the listener last heard. a->lock held. */
static void streamStopAica(DcAudioSystem* a, DcInstance* in) {
    waitIo(a, in);
    if (!in->started) { in->startPending = false; return; }
    uint32_t heard = streamPlayingSample(a, in);
    snd_stream_stop(in->stream);
    in->started = false;
    if (!in->finished) streamSeekSample(a, in, heard);
}

static void stopInstance(DcAudioSystem* a, DcInstance* in) {
    if (!in->active) return;
    if (getenv("BS_AUDIO_TRACE")) {
        UtdcAudEntry* e = &a->entries[in->entry];
        logInfo("DC audio: sound %d ended after %u ms (length %u ms, %s)\n", (int) in->soundIndex, (unsigned) (nowMs() - in->startMs),
                (unsigned) ((uint64_t) e->samples * 1000u / e->rate), in->kind == UTDC_AUDKIND_SFX ? "sfx" : "stream");
    }
    if (in->kind == UTDC_AUDKIND_SFX) {
        if (in->chn >= 0) snd_sfx_stop(in->chn);
        int idx = (int) (in - a->inst);
        for (int k = 0; k < a->sfxChanCount; k++) if (a->sfxChanOwner[k] == idx) a->sfxChanOwner[k] = -1;
    } else {
        mutex_lock(&a->lock);
        waitIo(a, in);
        in->startPending = false;
        if (in->stream != SND_STREAM_INVALID) {
            if (in->started) snd_stream_stop(in->stream);
            in->started = false;
            snd_stream_destroy(in->stream);
            in->stream = SND_STREAM_INVALID;
        }
        if (in->file != FILEHND_INVALID) { fs_close(in->file); in->file = FILEHND_INVALID; }
        free(in->buf); in->buf = nullptr;
        mutex_unlock(&a->lock);
    }
    in->active = false;
}

static int activeStreams(DcAudioSystem* a) {
    int n = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) if (a->inst[i].active && a->inst[i].kind == UTDC_AUDKIND_STREAM) n++;
    return n;
}

/* ----------------------------------------------------------------------- */
/* Vtable                                                                  */
/* ----------------------------------------------------------------------- */

static void dcaInit(AudioSystem* audio, DataWin* dataWin, MAYBE_UNUSED FileSystem* fs) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    arrput(a->base.audioGroups, dataWin);
    a->base.dw = dataWin;
}

static void dcaDestroy(AudioSystem* audio) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    a->quit = true;
    if (a->thread) thd_join(a->thread, nullptr);
    for (int i = 0; i < MAX_INSTANCES; i++) stopInstance(a, &a->inst[i]);
    snd_sfx_unload_all();
}

static DcInstance* allocInstance(DcAudioSystem* a, int32_t priority) {
    for (int i = 0; i < MAX_INSTANCES; i++) if (!a->inst[i].active) return &a->inst[i];
    /* steal the oldest finished-or-oldest sfx */
    DcInstance* best = nullptr;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        DcInstance* in = &a->inst[i];
        if (in->kind != UTDC_AUDKIND_SFX) continue;
        if (!best || in->startMs < best->startMs) best = in;
    }
    (void) priority;
    if (best) stopInstance(a, best);
    return best;
}

/* Picks one of our reserved AICA channels for a sound effect. KOS's own picker just rotates
   through the channels, so a frequent sound (text blips) would cut off a long one that is still
   playing (e.g. Flowey's laugh during his dialogue). */
static int pickSfxChannel(DcAudioSystem* a, DcInstance* forInst) {
    int idx = (int) (forInst - a->inst);
    int best = -1;
    for (int k = 0; k < a->sfxChanCount; k++) {
        int o = a->sfxChanOwner[k];
        if (o < 0 || !a->inst[o].active || a->inst[o].kind != UTDC_AUDKIND_SFX || a->inst[o].chn != a->sfxChan[k]) { best = k; break; }
    }
    if (best < 0) {
        /* all busy: take the channel of the oldest non-looping effect (or the oldest one) */
        uint64_t oldest = ~0ull;
        for (int pass = 0; pass < 2 && best < 0; pass++)
            for (int k = 0; k < a->sfxChanCount; k++) {
                DcInstance* o = &a->inst[a->sfxChanOwner[k]];
                if (pass == 0 && o->loop) continue;
                if (o->startMs < oldest) { oldest = o->startMs; best = k; }
            }
        if (best >= 0) stopInstance(a, &a->inst[a->sfxChanOwner[best]]);
    }
    if (best < 0) return -1;
    a->sfxChanOwner[best] = (int8_t) idx;
    return a->sfxChan[best];
}

static int32_t dcaPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    int32_t entry = -1;
    float vol = 1.0f, pitch = 1.0f;
    if (soundIndex >= DC_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundIndex - DC_AUDIO_STREAM_INDEX_BASE;
        if (slot < 0 || slot >= MAX_STREAM_SLOTS || !a->streams[slot].used) return -1;
        entry = a->streams[slot].entry;
    } else {
        if (soundIndex < 0 || (uint32_t) soundIndex >= a->hdr.soundCount) return -1;
        entry = a->soundToEntry[soundIndex];
        DataWin* dw = a->base.dw;
        if (dw && (uint32_t) soundIndex < dw->sond.count) {
            Sound* s = &dw->sond.sounds[soundIndex];
            vol = s->volume > 0.0f ? s->volume : 1.0f;
            pitch = s->pitch > 0.0f ? s->pitch : 1.0f;
        }
    }
    if (entry < 0 || (uint32_t) entry >= a->hdr.entryCount) return -1;
    UtdcAudEntry* e = &a->entries[entry];

    if (e->kind == UTDC_AUDKIND_STREAM && activeStreams(a) >= MAX_ACTIVE_STREAMS) {
        /* stop the oldest stream to make room */
        DcInstance* old = nullptr;
        for (int i = 0; i < MAX_INSTANCES; i++) {
            DcInstance* in = &a->inst[i];
            if (in->active && in->kind == UTDC_AUDKIND_STREAM && (!old || in->startMs < old->startMs)) old = in;
        }
        if (old) stopInstance(a, old);
    }

    DcInstance* in = allocInstance(a, priority);
    if (!in) return -1;
    memset(in, 0, sizeof *in);
    in->id = DC_SOUND_INSTANCE_ID_BASE + a->nextId++;
    in->soundIndex = soundIndex;
    in->entry = entry;
    in->kind = e->kind;
    in->loop = loop;
    in->baseVolume = vol;
    in->gain = in->startGain = in->targetGain = 1.0f;
    in->pitch = pitch;
    in->startMs = nowMs();
    in->chn = -1;
    in->stream = SND_STREAM_INVALID;
    in->file = FILEHND_INVALID;
    in->owner = a;

    if (e->kind == UTDC_AUDKIND_SFX) {
        sfxhnd_t h = sfxEnsure(a, entry);
        if (h == SFXHND_INVALID) { logWarn("DC audio: could not load sound %d (entry %d, %u bytes)\n", soundIndex, entry, e->size); return -1; }
        sfx_play_data_t pd;
        memset(&pd, 0, sizeof pd);
        pd.chn = a->sfxChanCount > 0 ? pickSfxChannel(a, in) : -1;
        pd.idx = h;
        pd.vol = volToAica(effectiveGain(a, in));
        pd.pan = 128;
        pd.loop = loop ? 1 : 0;
        pd.freq = (int) (e->rate * pitch);
        in->chn = snd_sfx_play_ex(&pd);
        if (in->chn < 0) logWarn("DC audio: no AICA channel for sound %d\n", soundIndex);
        in->active = true;
        if (a->allPaused) { in->paused = true; in->pausedAtMs = in->startMs; if (in->chn >= 0) snd_sfx_stop(in->chn); }
    } else {
        in->file = fs_open(a->path, O_RDONLY);
        if (in->file == FILEHND_INVALID) return -1;
        in->buf = (uint8_t*) memalign(32, STREAM_PREFETCH);
        if (!in->buf) { fs_close(in->file); in->file = FILEHND_INVALID; return -1; }
        mutex_lock(&a->lock);
        in->active = true;
        /* KOS gives a new stream the lowest free channels; find out which, so pitch can be
           changed live by writing the channel frequency (no stream restart). */
        in->aicaCh[0] = snd_sfx_chn_alloc();
        in->aicaCh[1] = snd_sfx_chn_alloc();
        if (in->aicaCh[0] >= 0) snd_sfx_chn_free(in->aicaCh[0]);
        if (in->aicaCh[1] >= 0) snd_sfx_chn_free(in->aicaCh[1]);
        in->stream = snd_stream_alloc(streamCallback, STREAM_BUF_PER_CHANNEL);
        if (in->stream == SND_STREAM_INVALID) {
            mutex_unlock(&a->lock);
            in->active = false;
            fs_close(in->file); free(in->buf); in->buf = nullptr;
            logWarn("DC audio: no free stream for sound %d\n", soundIndex);
            return -1;
        }
        streamResetDecoder(in);
        if (a->allPaused) in->paused = true;
        else streamStartAica(a, in);
        logInfo("DC audio: stream '%s' started (%u Hz, %s, loop=%d)\n",
                e->nameOffset < a->hdr.nameBytes ? a->names + e->nameOffset : "?", e->rate, e->channels == 2 ? "stereo" : "mono", loop ? 1 : 0);
        mutex_unlock(&a->lock);
    }
    return in->id;
}

static DcInstance* byId(DcAudioSystem* a, int32_t id) {
    for (int i = 0; i < MAX_INSTANCES; i++) if (a->inst[i].active && a->inst[i].id == id) return &a->inst[i];
    return nullptr;
}

/* Calls fn for the instance with this id, or every instance of this sound index */
#define FOR_TARGETS(a, target, body) do { \
    for (int _i = 0; _i < MAX_INSTANCES; _i++) { \
        DcInstance* in = &(a)->inst[_i]; \
        if (!in->active) continue; \
        if ((target) >= DC_SOUND_INSTANCE_ID_BASE && (target) < DC_AUDIO_STREAM_INDEX_BASE) { if (in->id != (target)) continue; } \
        else if (in->soundIndex != (target)) continue; \
        body; \
    } } while (0)

static bool instancePlaying(DcAudioSystem* a, DcInstance* in) {
    if (!in->active) return false;
    if (in->paused) return true;
    if (in->kind == UTDC_AUDKIND_STREAM) return in->loop || !in->finished || !in->drainUntilMs || nowMs() < in->drainUntilMs;
    if (in->loop) return true;
    UtdcAudEntry* e = &a->entries[in->entry];
    float secs = (float) e->samples / ((float) e->rate * (in->pitch > 0 ? in->pitch : 1.0f));
    return (float) (nowMs() - in->startMs) < secs * 1000.0f + 30.0f;
}

static void applyVolume(DcAudioSystem* a, DcInstance* in) {
    int v = volToAica(effectiveGain(a, in));
    if (in->kind == UTDC_AUDKIND_SFX) { if (in->chn >= 0 && !in->paused) aicaChannelUpdate(in->chn, AICA_CH_UPDATE_SET_VOL, v, 0); }
    else if (in->stream != SND_STREAM_INVALID) { mutex_lock(&a->lock); snd_stream_volume(in->stream, v); mutex_unlock(&a->lock); }
}

static void dcaUpdate(AudioSystem* audio, float dt) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    bool groupChanged = AudioSystem_updateGroupGains(audio, dt);
    for (int i = 0; i < MAX_INSTANCES; i++) {
        DcInstance* in = &a->inst[i];
        if (!in->active) continue;
        bool changed = groupChanged;
        if (in->fadeTime > 0.0f) {
            in->fadeTime -= dt;
            if (in->fadeTime <= 0.0f) { in->fadeTime = 0; in->gain = in->targetGain; }
            else in->gain = in->startGain + (in->targetGain - in->startGain) * (1.0f - in->fadeTime / in->fadeTotal);
            changed = true;
        }
        if (changed) applyVolume(a, in);
        if (!instancePlaying(a, in)) stopInstance(a, in);
    }
}

static void dcaSetSoundSpatial(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t id, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float z, MAYBE_UNUSED float vx, MAYBE_UNUSED float vy, MAYBE_UNUSED float vz, MAYBE_UNUSED float r, MAYBE_UNUSED float m, MAYBE_UNUSED float f) {}
static void dcaSetListenerPosition(AudioSystem* audio, float x, float y, float z) { audio->listenerX = x; audio->listenerY = y; audio->listenerZ = z; }

static void dcaStopSound(AudioSystem* audio, int32_t target) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    FOR_TARGETS(a, target, stopInstance(a, in));
}

static void dcaStopAll(AudioSystem* audio) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    for (int i = 0; i < MAX_INSTANCES; i++) stopInstance(a, &a->inst[i]);
}

static bool dcaIsPlaying(AudioSystem* audio, int32_t target) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    bool playing = false;
    FOR_TARGETS(a, target, if (instancePlaying(a, in)) playing = true);
    return playing;
}

static void pauseInstance(DcAudioSystem* a, DcInstance* in) {
    if (in->paused) return;
    in->paused = true;
    in->pausedAtMs = nowMs();
    if (in->kind == UTDC_AUDKIND_SFX) { if (in->chn >= 0) snd_sfx_stop(in->chn); }
    else if (in->stream != SND_STREAM_INVALID) { mutex_lock(&a->lock); streamStopAica(a, in); mutex_unlock(&a->lock); }
}

static void resumeInstance(DcAudioSystem* a, DcInstance* in) {
    if (!in->paused) return;
    in->paused = false;
    if (in->kind == UTDC_AUDKIND_SFX) {
        /* AICA one-shots can't resume mid-sample; restart looping sounds, drop one-shots */
        if (in->loop) {
            sfxhnd_t h = sfxEnsure(a, in->entry);
            sfx_play_data_t pd; memset(&pd, 0, sizeof pd);
            pd.chn = a->sfxChanCount > 0 ? pickSfxChannel(a, in) : -1; pd.idx = h; pd.vol = volToAica(effectiveGain(a, in)); pd.pan = 128; pd.loop = 1;
            pd.freq = (int) (a->entries[in->entry].rate * in->pitch);
            in->chn = snd_sfx_play_ex(&pd);
        } else {
            in->active = false;
        }
    } else if (in->stream != SND_STREAM_INVALID) {
        mutex_lock(&a->lock);
        if (!in->started && !in->startPending && !in->finished) streamStartAica(a, in);
        mutex_unlock(&a->lock);
    }
}

static void dcaPauseSound(AudioSystem* audio, int32_t target) { DcAudioSystem* a = (DcAudioSystem*) audio; FOR_TARGETS(a, target, pauseInstance(a, in)); }
static void dcaResumeSound(AudioSystem* audio, int32_t target) { DcAudioSystem* a = (DcAudioSystem*) audio; FOR_TARGETS(a, target, resumeInstance(a, in)); }
static void dcaPauseAll(AudioSystem* audio) { DcAudioSystem* a = (DcAudioSystem*) audio; a->allPaused = true; for (int i = 0; i < MAX_INSTANCES; i++) if (a->inst[i].active) pauseInstance(a, &a->inst[i]); }
static void dcaResumeAll(AudioSystem* audio) { DcAudioSystem* a = (DcAudioSystem*) audio; a->allPaused = false; for (int i = 0; i < MAX_INSTANCES; i++) if (a->inst[i].active) resumeInstance(a, &a->inst[i]); }
static void dcaSuspend(AudioSystem* audio) { dcaPauseAll(audio); }
static void dcaResume(AudioSystem* audio) { dcaResumeAll(audio); }

static void dcaSetSoundGain(AudioSystem* audio, int32_t target, float gain, uint32_t timeMs) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    if (gain < 0) gain = 0;
    FOR_TARGETS(a, target, {
        if (timeMs == 0) { in->gain = gain; in->fadeTime = 0; applyVolume(a, in); }
        else { in->startGain = in->gain; in->targetGain = gain; in->fadeTotal = in->fadeTime = (float) timeMs / 1000.0f; }
    });
}

static float dcaGetSoundGain(AudioSystem* audio, int32_t target) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    float g = 1.0f;
    FOR_TARGETS(a, target, { g = in->gain; break; });
    return g;
}

/* Restarts a music stream at a new playback rate, continuing from the same position.
   (KOS streams can't change frequency while running.) */
/* Applies a new pitch to a running stream by changing the AICA channel frequency in place.
   The position estimate is rebased so track position / pause stay exact. */
static void restartStreamAtPitch(DcAudioSystem* a, DcInstance* in, float oldPitch) {
    UtdcAudEntry* e = &a->entries[in->entry];
    mutex_lock(&a->lock);
    if (in->stream != SND_STREAM_INVALID && in->started) {
        float newPitch = in->pitch;
        in->pitch = oldPitch;
        uint32_t heard = streamPlayingSample(a, in);
        in->pitch = newPitch;
        in->playStartSample = heard;
        in->playStartMs = timer_ms_gettime64();
        uint32_t freq = (uint32_t) ((float) e->rate * newPitch);
        if (freq < 1000) freq = 1000;
        if (freq > 88200) freq = 88200;
        if (in->aicaCh[0] >= 0) {
            aicaChannelUpdate(in->aicaCh[0], AICA_CH_UPDATE_SET_FREQ, 0, freq);
            if (e->channels == 2 && in->aicaCh[1] >= 0) aicaChannelUpdate(in->aicaCh[1], AICA_CH_UPDATE_SET_FREQ, 0, freq);
        } else {
            streamStopAica(a, in);
            streamStartAica(a, in);
        }
    }
    mutex_unlock(&a->lock);
}

static void dcaSetSoundPitch(AudioSystem* audio, int32_t target, float pitch) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    if (pitch <= 0.0f) pitch = 0.01f;
    FOR_TARGETS(a, target, {
        bool changed = fabsf(in->pitch - pitch) > 0.0005f;
        float oldPitch = in->pitch;
        in->pitch = pitch;
        if (changed) {
            if (in->kind == UTDC_AUDKIND_SFX) {
                if (in->chn >= 0) aicaChannelUpdate(in->chn, AICA_CH_UPDATE_SET_FREQ, 0, (uint32_t) (a->entries[in->entry].rate * pitch));
            } else {
                restartStreamAtPitch(a, in, oldPitch);
            }
        }
    });
}

static float dcaGetSoundPitch(AudioSystem* audio, int32_t target) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    float p = 1.0f;
    FOR_TARGETS(a, target, { p = in->pitch; break; });
    return p;
}

static float dcaGetTrackPosition(AudioSystem* audio, int32_t target) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    float pos = 0.0f;
    FOR_TARGETS(a, target, {
        UtdcAudEntry* e = &a->entries[in->entry];
        if (in->kind == UTDC_AUDKIND_STREAM) { mutex_lock(&a->lock); pos = (float) streamPlayingSample(a, in) / (float) e->rate; mutex_unlock(&a->lock); }
        else pos = (float) ((in->paused ? in->pausedAtMs : nowMs()) - in->startMs) / 1000.0f * in->pitch;
        break;
    });
    return pos;
}

static void dcaSetTrackPosition(AudioSystem* audio, int32_t target, float seconds) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    FOR_TARGETS(a, target, {
        if (in->kind == UTDC_AUDKIND_STREAM) {
            UtdcAudEntry* e = &a->entries[in->entry];
            uint32_t tgt = (uint32_t) (seconds * (float) e->rate);
            if (tgt >= e->samples) tgt = 0;
            if (e->channels != 2) tgt &= ~1u;
            mutex_lock(&a->lock);
            waitIo(a, in);
            bool was = in->started || in->startPending;
            if (in->started) { snd_stream_stop(in->stream); in->started = false; }
            in->startPending = false;
            in->cpCount = 0; /* checkpoints may be ahead of the target */
            streamSeekSample(a, in, tgt);
            if (was) streamStartAica(a, in);
            else in->playStartSample = in->decSample;
            mutex_unlock(&a->lock);
        }
    });
}

static float dcaGetSoundLength(AudioSystem* audio, int32_t target) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    int32_t entry = -1;
    if (target >= DC_AUDIO_STREAM_INDEX_BASE) {
        int32_t s = target - DC_AUDIO_STREAM_INDEX_BASE;
        if (s >= 0 && s < MAX_STREAM_SLOTS && a->streams[s].used) entry = a->streams[s].entry;
    } else if (target >= DC_SOUND_INSTANCE_ID_BASE) {
        DcInstance* in = byId(a, target);
        if (in) entry = in->entry;
    } else if (target >= 0 && (uint32_t) target < a->hdr.soundCount) entry = a->soundToEntry[target];
    if (entry < 0) return 0.0f;
    return (float) a->entries[entry].samples / (float) a->entries[entry].rate;
}

static void dcaSetMasterGain(AudioSystem* audio, float gain) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    a->masterGain = gain < 0 ? 0 : gain;
    for (int i = 0; i < MAX_INSTANCES; i++) if (a->inst[i].active) applyVolume(a, &a->inst[i]);
}
static void dcaSetMasterGainForListener(AudioSystem* audio, float gain, MAYBE_UNUSED int32_t listener) { dcaSetMasterGain(audio, gain); }
static void dcaSetChannelCount(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t c) {}
static void dcaSetGroupGain(AudioSystem* audio, int32_t group, float gain, uint32_t timeMs) { AudioSystem_setGroupGain(audio, group, gain, timeMs); }
static void dcaGroupLoad(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t g) {}
static bool dcaGroupIsLoaded(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t g) { return true; }

static int32_t dcaCreateStream(AudioSystem* audio, const char* filename) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    char key[96];
    makeKey(filename, key, sizeof key);
    int32_t entry = findEntryByKey(a, key);
    if (entry < 0) { logWarn("DC audio: stream '%s' (%s) not on disc\n", filename, key); return -1; }
    for (int i = 0; i < MAX_STREAM_SLOTS; i++) {
        if (!a->streams[i].used) {
            a->streams[i].used = true;
            a->streams[i].entry = entry;
            return DC_AUDIO_STREAM_INDEX_BASE + i;
        }
    }
    return -1;
}

static bool dcaDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    int32_t s = streamIndex - DC_AUDIO_STREAM_INDEX_BASE;
    if (s < 0 || s >= MAX_STREAM_SLOTS || !a->streams[s].used) return false;
    dcaStopSound(audio, streamIndex);
    a->streams[s].used = false;
    return true;
}

static int32_t dcaPlayEncoded(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED const uint8_t* data, MAYBE_UNUSED size_t bytes, MAYBE_UNUSED bool loop) { return -1; }

static AudioSystemVtable dcaVtable = {
    .init = dcaInit, .destroy = dcaDestroy, .update = dcaUpdate, .playSound = dcaPlaySound,
    .setSoundSpatial = dcaSetSoundSpatial, .setListenerPosition = dcaSetListenerPosition,
    .stopSound = dcaStopSound, .stopAll = dcaStopAll, .isPlaying = dcaIsPlaying,
    .pauseSound = dcaPauseSound, .resumeSound = dcaResumeSound, .pauseAll = dcaPauseAll, .resumeAll = dcaResumeAll,
    .suspend = dcaSuspend, .resume = dcaResume,
    .setSoundGain = dcaSetSoundGain, .getSoundGain = dcaGetSoundGain,
    .setSoundPitch = dcaSetSoundPitch, .getSoundPitch = dcaGetSoundPitch,
    .getTrackPosition = dcaGetTrackPosition, .setTrackPosition = dcaSetTrackPosition, .getSoundLength = dcaGetSoundLength,
    .setMasterGain = dcaSetMasterGain, .setMasterGainForListener = dcaSetMasterGainForListener,
    .setChannelCount = dcaSetChannelCount, .setGroupGain = dcaSetGroupGain,
    .groupLoad = dcaGroupLoad, .groupIsLoaded = dcaGroupIsLoaded,
    .createStream = dcaCreateStream, .destroyStream = dcaDestroyStream, .playEncoded = dcaPlayEncoded,
};

AudioSystem* DcAudioSystem_create(const char* audioPath) {
    DcAudioSystem* a = (DcAudioSystem*) safeCalloc(1, sizeof(DcAudioSystem));
    a->base.vtable = &dcaVtable;
    a->masterGain = 1.0f;
    snprintf(a->path, sizeof a->path, "%s", audioPath);
    a->file = fs_open(audioPath, O_RDONLY);
    if (a->file == FILEHND_INVALID) { logError("DC audio: cannot open %s\n", audioPath); free(a); return nullptr; }
    if (fs_read(a->file, &a->hdr, sizeof a->hdr) != (ssize_t) sizeof a->hdr || a->hdr.magic != UTDC_AUD_MAGIC || a->hdr.version != UTDC_FORMAT_VERSION) {
        logError("DC audio: %s is not a valid AUDIO.BIN\n", audioPath);
        fs_close(a->file); free(a); return nullptr;
    }
    a->soundToEntry = (int32_t*) safeMalloc(a->hdr.soundCount * sizeof(int32_t));
    a->entries = (UtdcAudEntry*) safeMalloc(a->hdr.entryCount * sizeof(UtdcAudEntry));
    a->names = (char*) safeMalloc(a->hdr.nameBytes + 1);
    fs_read(a->file, a->soundToEntry, a->hdr.soundCount * sizeof(int32_t));
    fs_read(a->file, a->entries, a->hdr.entryCount * sizeof(UtdcAudEntry));
    fs_read(a->file, a->names, a->hdr.nameBytes);
    a->names[a->hdr.nameBytes] = 0;
    a->sfx = (SfxCacheEntry*) safeMalloc(a->hdr.entryCount * sizeof(SfxCacheEntry));
    for (uint32_t i = 0; i < a->hdr.entryCount; i++) a->sfx[i].hnd = SFXHND_INVALID;

    snd_stream_init_ex(2, STREAM_BUF_PER_CHANNEL);
    /* Reserve the top AICA channels for sound effects. Streams must keep low channel numbers:
       KOS starts a stereo stream's two channels with a 32-bit channel mask. */
    {
        int got[64], n = 0, c;
        while (n < 64 && (c = snd_sfx_chn_alloc()) >= 0) got[n++] = c;
        for (int i = 0; i < n; i++) {
            if (got[i] >= 64 - SFX_CHANNELS && a->sfxChanCount < SFX_CHANNELS) {
                a->sfxChan[a->sfxChanCount] = got[i];
                a->sfxChanOwner[a->sfxChanCount] = -1;
                a->sfxChanCount++;
            } else {
                snd_sfx_chn_free(got[i]);
            }
        }
    }
    mutex_init(&a->lock, MUTEX_TYPE_RECURSIVE);
    g_audio = a;
    a->thread = thd_create(0, streamThread, a);
    logInfo("DC audio: %u sounds, %u entries\n", a->hdr.soundCount, a->hdr.entryCount);
    return &a->base;
}
