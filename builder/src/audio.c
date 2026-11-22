/* Converts every sound of the game into AICA ADPCM and writes AUDIO.BIN.
 *
 *  - Embedded WAVs (AUDO chunk) and short OGGs  -> mono sound effects, uploaded
 *    to the 2 MB sound RAM on demand by the runner.
 *  - Long OGGs (music)                         -> streams read from disc.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "build.h"
#include "../../common/utdc_formats.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb/stb_vorbis.c"

#define SFX_MAX_SECONDS 6.0

typedef struct {
    int16_t* pcm;      /* interleaved */
    int channels;
    int rate;
    long frames;
} Pcm;

/* ---------------- decoding ---------------- */

static uint32_t r32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int decode_wav(const uint8_t* d, size_t n, Pcm* out) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
    size_t off = 12;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t* data = NULL; size_t dataLen = 0;
    while (off + 8 <= n) {
        uint32_t len = r32(d + off + 4);
        const uint8_t* body = d + off + 8;
        if (off + 8 + len > n) len = (uint32_t)(n - off - 8);
        if (!memcmp(d + off, "fmt ", 4) && len >= 16) {
            fmt = r16(body); ch = r16(body + 2); rate = (int)r32(body + 4); bits = r16(body + 14);
        } else if (!memcmp(d + off, "data", 4)) {
            data = body; dataLen = len;
        }
        off += 8 + len + (len & 1);
    }
    if (!data || ch < 1 || ch > 2 || rate <= 0) return -1;
    if (fmt != 1 && fmt != 0xFFFE && fmt != 3) return -1;
    int bps = bits / 8;
    if (bps < 1) return -1;
    long frames = (long)(dataLen / (size_t)(bps * ch));
    out->pcm = (int16_t*)malloc((size_t)frames * ch * sizeof(int16_t) + 16);
    out->channels = ch; out->rate = rate; out->frames = frames;
    for (long i = 0; i < frames * ch; i++) {
        const uint8_t* s = data + (size_t)i * bps;
        int v;
        if (fmt == 3 && bits == 32) { float f; memcpy(&f, s, 4); v = (int)lrintf(f * 32767.0f); }
        else if (bits == 8) v = ((int)s[0] - 128) << 8;
        else if (bits == 16) v = (int16_t)r16(s);
        else if (bits == 24) v = (int16_t)r16(s + 1);
        else v = (int16_t)r16(s + 2);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out->pcm[i] = (int16_t)v;
    }
    return 0;
}

static int decode_ogg(const uint8_t* d, size_t n, Pcm* out) {
    int ch = 0, rate = 0;
    short* pcm = NULL;
    int frames = stb_vorbis_decode_memory(d, (int)n, &ch, &rate, &pcm);
    if (frames < 0 || !pcm) return -1;
    if (ch > 2) { /* downmix to stereo */
        short* st = (short*)malloc((size_t)frames * 2 * sizeof(short));
        for (int i = 0; i < frames; i++) { st[i * 2] = pcm[i * ch]; st[i * 2 + 1] = pcm[i * ch + 1]; }
        free(pcm); pcm = st; ch = 2;
    }
    out->pcm = pcm; out->channels = ch; out->rate = rate; out->frames = frames;
    return 0;
}

static uint8_t* read_file(const char* path, size_t* outLen) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *outLen = (size_t)n;
    return b;
}

/* ---------------- processing ---------------- */

static void to_mono(Pcm* p) {
    if (p->channels == 1) return;
    for (long i = 0; i < p->frames; i++) p->pcm[i] = (int16_t)(((int)p->pcm[i * 2] + p->pcm[i * 2 + 1]) / 2);
    p->channels = 1;
}

/* Polyphase windowed-sinc resampler (Blackman window, table driven) */
#define RS_PHASES 512
static void resample(Pcm* p, int newRate) {
    if (p->rate <= newRate || p->frames == 0) return;
    double ratio = (double)p->rate / newRate;
    long outFrames = (long)((double)p->frames / ratio);
    int ch = p->channels;
    const int HALF = (int)ceil(5 * ratio);          /* taps on each side */
    const int NT = 2 * HALF;
    double cutoff = 0.92 / ratio;
    float* table = (float*)malloc((size_t)(RS_PHASES + 1) * NT * sizeof(float));
    for (int ph = 0; ph <= RS_PHASES; ph++) {
        double frac = (double)ph / RS_PHASES;
        double sum = 0;
        float* w = table + (size_t)ph * NT;
        for (int t = 0; t < NT; t++) {
            double x = (t - HALF + 1) - frac;
            double span = HALF;
            double v = 0;
            if (fabs(x) < span) {
                double sx = M_PI * x * cutoff;
                double sinc = fabs(sx) < 1e-9 ? 1.0 : sin(sx) / sx;
                double win = 0.42 + 0.5 * cos(M_PI * x / span) + 0.08 * cos(2 * M_PI * x / span);
                v = sinc * win;
            }
            w[t] = (float)v; sum += v;
        }
        for (int t = 0; t < NT; t++) w[t] = (float)(w[t] / sum);
    }
    int16_t* o = (int16_t*)malloc((size_t)outFrames * ch * sizeof(int16_t) + 16);
    for (long i = 0; i < outFrames; i++) {
        double center = i * ratio;
        long c0 = (long)center;
        int ph = (int)((center - c0) * RS_PHASES + 0.5);
        const float* w = table + (size_t)ph * NT;
        long base = c0 - HALF + 1;
        for (int c = 0; c < ch; c++) {
            float acc = 0;
            if (base >= 0 && base + NT < p->frames) {
                const int16_t* src = p->pcm + base * ch + c;
                for (int t = 0; t < NT; t++) acc += w[t] * src[t * ch];
            } else {
                for (int t = 0; t < NT; t++) {
                    long k = base + t;
                    if (k < 0) k = 0;
                    if (k >= p->frames) k = p->frames - 1;
                    acc += w[t] * p->pcm[k * ch + c];
                }
            }
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            o[i * ch + c] = (int16_t)lrintf(acc);
        }
    }
    free(table);
    free(p->pcm);
    p->pcm = o; p->frames = outFrames; p->rate = newRate;
}

/* AICA ADPCM encoder (public-domain algorithm by superctr, as used by KOS wav2adpcm) */
#define CLAMPI(x, lo, hi) ((x) > (hi) ? (hi) : ((x) < (lo) ? (lo) : (x)))
static void ymz_step(uint8_t step, int16_t* history, int16_t* stepSize) {
    static const int stepTable[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    int sign = step & 8, delta = step & 7;
    int diff = ((1 + (delta << 1)) * *stepSize) >> 3;
    int newval = *history;
    int nstep = (stepTable[delta] * *stepSize) >> 8;
    diff = CLAMPI(diff, 0, 32767);
    if (sign) newval -= diff; else newval += diff;
    *stepSize = (int16_t)CLAMPI(nstep, 127, 24576);
    *history = (int16_t)CLAMPI(newval, -32768, 32767);
}

/* Encodes one channel (stride = channel count) into nibbles; out has (frames+1)/2 bytes */
static void encode_channel(const int16_t* pcm, long frames, int stride, uint8_t* nibbles) {
    int16_t stepSize = 127, history = 0;
    for (long i = 0; i < frames; i++) {
        int step = (pcm[i * stride] & -8) - history;
        uint32_t s = (uint32_t)(abs(step) << 16) / (uint32_t)(stepSize << 14);
        s = CLAMPI(s, 0u, 7u);
        if (step < 0) s |= 8;
        nibbles[i] = (uint8_t)s;
        ymz_step((uint8_t)s, &history, &stepSize);
    }
}

/* Produces ADPCM bytes. Mono: low nibble = even sample. Stereo: byte i = L[i]<<4 | R[i]. */
static uint8_t* encode_adpcm(const Pcm* p, uint32_t* outSize, long* outFrames) {
    long frames = (p->frames + 63) & ~63L; /* pad to keep sizes 32-byte aligned */
    int16_t* padded = (int16_t*)calloc((size_t)frames * p->channels, sizeof(int16_t));
    memcpy(padded, p->pcm, (size_t)p->frames * p->channels * sizeof(int16_t));
    uint8_t* nl = (uint8_t*)malloc((size_t)frames);
    uint8_t* out;
    uint32_t size;
    if (p->channels == 1) {
        encode_channel(padded, frames, 1, nl);
        size = (uint32_t)(frames / 2);
        out = (uint8_t*)malloc(size);
        for (long i = 0; i < frames; i += 2) out[i / 2] = (uint8_t)(nl[i] | (nl[i + 1] << 4));
    } else {
        uint8_t* nr = (uint8_t*)malloc((size_t)frames);
        encode_channel(padded, frames, 2, nl);
        encode_channel(padded + 1, frames, 2, nr);
        size = (uint32_t)frames;
        out = (uint8_t*)malloc(size);
        for (long i = 0; i < frames; i++) out[i] = (uint8_t)((nl[i] << 4) | nr[i]);
        free(nr);
    }
    free(nl); free(padded);
    *outSize = size; *outFrames = frames;
    return out;
}

/* ---------------- AUDIO.BIN assembly ---------------- */

typedef struct {
    UtdcAudEntry e;
    uint8_t* data;
    char key[64];
    char srcKey[300];  /* dedupe key */
} Entry;

void make_stream_key(const char* file, char* out, size_t outLen) {
    const char* base = file;
    for (const char* c = file; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    char tmp[256]; size_t i = 0;
    for (; base[i] && i < sizeof(tmp) - 1; i++) tmp[i] = (char)tolower((unsigned char)base[i]);
    tmp[i] = 0;
    char* dot = strrchr(tmp, '.');
    if (dot) *dot = 0;
    const char* k = tmp;
    if (!strncmp(k, "mus_", 4)) k += 4;
    snprintf(out, outLen, "%s", k);
}

static int file_exists_ci(const char* dir, const char* name, char* outPath, size_t outLen) {
    snprintf(outPath, outLen, "%s/%s", dir, name);
    FILE* f = fopen(outPath, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

int build_audio(BuildContext* ctx, const DataWinFile* dw, const BuildOptions* opts, const char* outPath) {
    int musicRate = opts->musicRate > 0 ? opts->musicRate : 44100;
    int sfxRate = opts->sfxRate > 0 ? opts->sfxRate : 44100;
    Entry* entries = (Entry*)calloc(dw->soundCount + 1, sizeof(Entry));
    int32_t* map = (int32_t*)malloc((dw->soundCount + 1) * sizeof(int32_t));
    int count = 0;
    uint64_t rawBytes = 0;

    for (uint32_t i = 0; i < dw->soundCount; i++) {
        map[i] = -1;
        if (build_cancelled(ctx)) goto fail;
        const DwSound* s = &dw->sound[i];
        build_progress(ctx, 0.40 + 0.45 * i / (dw->soundCount ? dw->soundCount : 1), "Converting audio %u/%u: %s", i + 1, dw->soundCount, s->name);

        /* bit 0 = embedded WAV, bit 1 = compressed (OGG stored in AUDO); both live inside data.win */
        int embedded = (s->flags & 3) != 0 && s->audioFile >= 0 && (uint32_t) s->audioFile < dw->audioCount;
        char srcKey[300];
        if (embedded) snprintf(srcKey, sizeof srcKey, "#audo%d", s->audioFile);
        else snprintf(srcKey, sizeof srcKey, "%s", s->file);
        int found = -1;
        for (int k = 0; k < count; k++) if (!strcmp(entries[k].srcKey, srcKey)) { found = k; break; }
        if (found >= 0) { map[i] = found; continue; }

        Pcm pcm = {0};
        int ok = -1;
        int isOgg = 0;
        if (embedded) {
            if (s->audioFile < 0 || (uint32_t)s->audioFile >= dw->audioCount) continue;
            const DwAudio* a = &dw->audio[s->audioFile];
            const uint8_t* d = dw->data + a->offset;
            if (a->size >= 4 && !memcmp(d, "OggS", 4)) { ok = decode_ogg(d, a->size, &pcm); isOgg = 1; }
            else ok = decode_wav(d, a->size, &pcm);
        } else {
            if (!s->file || !*s->file) continue;
            char path[1024];
            if (!file_exists_ci(opts->gameDir, s->file, path, sizeof path)) {
                build_log(ctx, "warning: missing audio file %s", s->file);
                continue;
            }
            size_t n; uint8_t* buf = read_file(path, &n);
            if (!buf) continue;
            rawBytes += n;
            if (n >= 4 && !memcmp(buf, "OggS", 4)) { ok = decode_ogg(buf, n, &pcm); isOgg = 1; }
            else ok = decode_wav(buf, n, &pcm);
            free(buf);
        }
        if (ok != 0) { build_log(ctx, "warning: could not decode sound %s", s->name); continue; }
        (void)isOgg;

        double secs = (double)pcm.frames / pcm.rate;
        /* The AICA addresses a one-shot sample with 16-bit start/end registers, so a sound effect
           must be at most 65534 samples. Use the source rate (44.1 kHz) when it fits, else the
           highest rate that fits (down to 16 kHz); longer sounds are streamed from disc instead. */
        const double AICA_MAX_SAMPLES = 65534.0;
        int stream = !embedded && secs > SFX_MAX_SECONDS;
        if (embedded && secs > 20.0) stream = 1;
        int sfxTarget = sfxRate;
        if (!stream) {
            double fitRate = AICA_MAX_SAMPLES / (secs > 0 ? secs : 1);
            if (fitRate >= sfxRate) sfxTarget = sfxRate;
            else if (fitRate >= 16000.0) sfxTarget = (int) fitRate & ~1;
            else stream = 1;
        }
        if (stream) {
            resample(&pcm, musicRate);
            ctx->stats.musicSeconds += secs;
            ctx->stats.streamCount++;
        } else {
            to_mono(&pcm);
            resample(&pcm, sfxTarget);
            if (pcm.frames > 65472) pcm.frames = 65472; /* stays <= 65534 after 64-sample ADPCM padding */
            ctx->stats.sfxCount++;
        }
        Entry* e = &entries[count];
        long frames; uint32_t size;
        e->data = encode_adpcm(&pcm, &size, &frames);
        e->e.size = size; e->e.rate = (uint32_t)pcm.rate; e->e.samples = (uint32_t)frames;
        e->e.channels = (uint8_t)pcm.channels; e->e.kind = stream ? UTDC_AUDKIND_STREAM : UTDC_AUDKIND_SFX;
        e->e.nameOffset = 0xFFFFFFFFu;
        snprintf(e->srcKey, sizeof e->srcKey, "%s", srcKey);
        make_stream_key(embedded ? s->name : s->file, e->key, sizeof e->key);
        free(pcm.pcm);
        map[i] = count++;
    }

    {
        FILE* f = fopen(outPath, "wb");
        if (!f) { build_log(ctx, "error: cannot write %s", outPath); goto fail; }
        /* names */
        size_t nameBytes = 0;
        for (int k = 0; k < count; k++) nameBytes += strlen(entries[k].key) + 1;
        char* names = (char*)malloc(nameBytes + 1);
        size_t np = 0;
        for (int k = 0; k < count; k++) {
            entries[k].e.nameOffset = (uint32_t)np;
            size_t l = strlen(entries[k].key) + 1;
            memcpy(names + np, entries[k].key, l); np += l;
        }
        UtdcAudHeader h = {0};
        h.magic = UTDC_AUD_MAGIC; h.version = UTDC_FORMAT_VERSION;
        h.soundCount = dw->soundCount; h.entryCount = (uint32_t)count; h.nameBytes = (uint32_t)nameBytes;
        uint32_t off = (uint32_t)(sizeof h + dw->soundCount * 4 + count * sizeof(UtdcAudEntry) + nameBytes);
        off = (off + 31) & ~31u;
        for (int k = 0; k < count; k++) { entries[k].e.offset = off; off = (off + entries[k].e.size + 31) & ~31u; }
        fwrite(&h, sizeof h, 1, f);
        fwrite(map, 4, dw->soundCount, f);
        for (int k = 0; k < count; k++) fwrite(&entries[k].e, sizeof(UtdcAudEntry), 1, f);
        fwrite(names, 1, nameBytes, f);
        static const uint8_t zeros[32] = {0};
        long pos = ftell(f);
        fwrite(zeros, 1, (size_t)(((pos + 31) & ~31) - pos), f);
        for (int k = 0; k < count; k++) {
            fwrite(entries[k].data, 1, entries[k].e.size, f);
            pos = ftell(f);
            fwrite(zeros, 1, (size_t)(((pos + 31) & ~31) - pos), f);
        }
        uint64_t total = (uint64_t)ftell(f);
        int bad = ferror(f);
        fclose(f);
        free(names);
        if (bad) { build_log(ctx, "error: write failed for %s", outPath); goto fail; }
        build_log(ctx, "Audio: %d sound effects, %d music streams (%.0f min), AUDIO.BIN %.1f MB",
                  ctx->stats.sfxCount, ctx->stats.streamCount, ctx->stats.musicSeconds / 60.0, total / 1048576.0);
    }
    for (int k = 0; k < count; k++) free(entries[k].data);
    free(entries); free(map);
    (void)rawBytes;
    return 0;
fail:
    for (int k = 0; k < count; k++) free(entries[k].data);
    free(entries); free(map);
    return -1;
}
