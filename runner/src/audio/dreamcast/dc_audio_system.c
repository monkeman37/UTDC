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
#define MAX_ACTIVE_STREAMS 3       /* KOS supports 4; keep one spare */
#define STREAM_BUF_PER_CHANNEL (16 * 1024)
#define SFX_CACHE_BUDGET (1200 * 1024)

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
    uint32_t pos;            /* bytes consumed */
    bool finished;
    uint8_t* buf;
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

/* Runs on the stream thread (inside snd_stream_poll) with a->lock held */
static void* streamCallback(snd_stream_hnd_t hnd, int req, int* recv) {
    DcInstance* in = streamOwner(hnd);
    *recv = 0;
    if (!in || in->finished || in->paused) return nullptr;
    UtdcAudEntry* e = &g_audio->entries[in->entry];
    int want = req;
    if (want > STREAM_BUF_PER_CHANNEL * 2) want = STREAM_BUF_PER_CHANNEL * 2;
    want &= ~31;
    int have = 0;
    while (have < want) {
        uint32_t left = e->size - in->pos;
        if (left == 0) {
            if (in->loop) { in->pos = 0; continue; }
            in->finished = true;
            break;
        }
        uint32_t n = (uint32_t) (want - have);
        if (n > left) n = left;
        fs_seek(in->file, e->offset + in->pos, SEEK_SET);
        ssize_t got = fs_read(in->file, in->buf + have, n);
        if (got <= 0) { in->finished = true; break; }
        have += (int) got;
        in->pos += (uint32_t) got;
    }
    if (have < want && have > 0) { memset(in->buf + have, 0, (size_t) (((have + 31) & ~31) - have)); have = (have + 31) & ~31; }
    *recv = have;
    return have ? in->buf : nullptr;
}

static void* streamThread(void* p) {
    DcAudioSystem* a = (DcAudioSystem*) p;
    while (!a->quit) {
        mutex_lock(&a->lock);
        for (int i = 0; i < MAX_INSTANCES; i++) {
            DcInstance* in = &a->inst[i];
            if (in->active && in->kind == UTDC_AUDKIND_STREAM && in->stream != SND_STREAM_INVALID && !in->paused)
                snd_stream_poll(in->stream);
        }
        mutex_unlock(&a->lock);
        thd_sleep(15);
    }
    return nullptr;
}

static void stopInstance(DcAudioSystem* a, DcInstance* in) {
    if (!in->active) return;
    if (in->kind == UTDC_AUDKIND_SFX) {
        if (in->chn >= 0) snd_sfx_stop(in->chn);
    } else {
        mutex_lock(&a->lock);
        if (in->stream != SND_STREAM_INVALID) {
            snd_stream_stop(in->stream);
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
        if (h == SFXHND_INVALID) return -1;
        sfx_play_data_t pd;
        memset(&pd, 0, sizeof pd);
        pd.chn = -1;
        pd.idx = h;
        pd.vol = volToAica(effectiveGain(a, in));
        pd.pan = 128;
        pd.loop = loop ? 1 : 0;
        pd.freq = (int) (e->rate * pitch);
        in->chn = snd_sfx_play_ex(&pd);
        in->active = true;
        if (a->allPaused) { in->paused = true; in->pausedAtMs = in->startMs; if (in->chn >= 0) snd_sfx_stop(in->chn); }
    } else {
        in->file = fs_open(a->path, O_RDONLY);
        if (in->file == FILEHND_INVALID) return -1;
        in->buf = (uint8_t*) memalign(32, STREAM_BUF_PER_CHANNEL * 2);
        mutex_lock(&a->lock);
        in->active = true;
        in->stream = snd_stream_alloc(streamCallback, STREAM_BUF_PER_CHANNEL);
        if (in->stream == SND_STREAM_INVALID) {
            mutex_unlock(&a->lock);
            in->active = false;
            fs_close(in->file); free(in->buf);
            logWarn("DC audio: no free stream for sound %d\n", soundIndex);
            return -1;
        }
        snd_stream_start_adpcm(in->stream, (uint32_t) (e->rate * pitch), e->channels == 2);
        logInfo("DC audio: stream '%s' started (%u Hz, %s, loop=%d)\n",
                e->nameOffset < a->hdr.nameBytes ? a->names + e->nameOffset : "?", e->rate, e->channels == 2 ? "stereo" : "mono", loop ? 1 : 0);
        snd_stream_volume(in->stream, volToAica(effectiveGain(a, in)));
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
    if (in->kind == UTDC_AUDKIND_STREAM) return !in->finished || in->loop;
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
    else if (in->stream != SND_STREAM_INVALID) { mutex_lock(&a->lock); snd_stream_volume(in->stream, 0); mutex_unlock(&a->lock); }
}

static void resumeInstance(DcAudioSystem* a, DcInstance* in) {
    if (!in->paused) return;
    in->paused = false;
    if (in->kind == UTDC_AUDKIND_SFX) {
        /* AICA one-shots can't resume mid-sample; restart looping sounds, drop one-shots */
        if (in->loop) {
            sfxhnd_t h = sfxEnsure(a, in->entry);
            sfx_play_data_t pd; memset(&pd, 0, sizeof pd);
            pd.chn = -1; pd.idx = h; pd.vol = volToAica(effectiveGain(a, in)); pd.pan = 128; pd.loop = 1;
            pd.freq = (int) (a->entries[in->entry].rate * in->pitch);
            in->chn = snd_sfx_play_ex(&pd);
        } else {
            in->active = false;
        }
    } else applyVolume(a, in);
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

static void dcaSetSoundPitch(AudioSystem* audio, int32_t target, float pitch) {
    DcAudioSystem* a = (DcAudioSystem*) audio;
    if (pitch <= 0.0f) pitch = 0.01f;
    FOR_TARGETS(a, target, {
        in->pitch = pitch;
        if (in->kind == UTDC_AUDKIND_SFX && in->chn >= 0)
            aicaChannelUpdate(in->chn, AICA_CH_UPDATE_SET_FREQ, 0, (uint32_t) (a->entries[in->entry].rate * pitch));
        /* stream pitch takes effect the next time the stream is started */
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
        if (in->kind == UTDC_AUDKIND_STREAM) pos = (float) in->pos * 2.0f / (float) e->channels / (float) e->rate;
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
            uint32_t bytes = (uint32_t) (seconds * (float) e->rate * (float) e->channels / 2.0f) & ~31u;
            if (bytes >= e->size) bytes = 0;
            mutex_lock(&a->lock); in->pos = bytes; in->finished = false; mutex_unlock(&a->lock);
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
    mutex_init(&a->lock, MUTEX_TYPE_RECURSIVE);
    g_audio = a;
    a->thread = thd_create(0, streamThread, a);
    logInfo("DC audio: %u sounds, %u entries\n", a->hdr.soundCount, a->hdr.entryCount);
    return &a->base;
}
