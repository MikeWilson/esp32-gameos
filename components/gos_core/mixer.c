// mixer.c — 8-voice chip synth, 22050 Hz mono. The synth engine follows
// infinite-golf's proven design (Q32 phase accumulator, LFSR noise with
// sample-and-hold, ~2 ms attack + linear decay envelope), generalized to a
// voice pool with priority stealing and looping voices.
//
// Concurrency: game task (core 1) starts/stops voices, audio task (core 0)
// renders. Voice start writes fields with active=false, then flips active
// last; the render loop tolerates a voice appearing mid-chunk. No locks in
// the sample path.
#include "gos.h"
#include "gos_core.h"
#include <string.h>

#define NVOICES 8
#define VOICE_AMP 5500.0f
#define RATE 22050

typedef struct {
    const gos_note_t *seq;
    uint8_t count, idx;
    bool loop;
    uint8_t prio;
    volatile uint16_t vol;       // 0..256 live scale
    uint32_t pos, len, phase;
    uint16_t lfsr;
    int16_t nval;
    int hold;
    uint32_t start_ms;
    uint8_t gen;
    volatile bool active;
} voice_t;

static voice_t voices[NVOICES];
static volatile int master = 2;   // 0..3

static const float level_scale[4] = { 0.f, 0.35f, 0.65f, 1.f };

void gos_mixer_init(void) { memset(voices, 0, sizeof voices); }

void gos_audio_master(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    master = level;
}

int gos_audio_master_get(void) { return master; }

static int alloc_voice(uint8_t prio)
{
    int best = -1;
    uint32_t best_age = 0;
    for (int i = 0; i < NVOICES; i++)
        if (!voices[i].active) return i;
    // steal: lowest priority, then oldest
    uint8_t lowest = 255;
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].prio < lowest) lowest = voices[i].prio;
    if (lowest > prio) return -1;
    for (int i = 0; i < NVOICES; i++) {
        if (voices[i].prio != lowest) continue;
        uint32_t age = gos_time_ms() - voices[i].start_ms;
        if (best < 0 || age > best_age) { best = i; best_age = age; }
    }
    return best;
}

static int start(const gos_sfx_t *s, uint8_t vol, bool loop)
{
    if (!s || !s->count) return -1;
    int i = alloc_voice(s->prio);
    if (i < 0) return -1;
    voice_t *v = &voices[i];
    v->active = false;
    __asm__ volatile("" ::: "memory");
    v->seq = s->notes;
    v->count = s->count;
    v->idx = 0;
    v->loop = loop;
    v->prio = s->prio;
    v->vol = vol + 1;
    v->pos = 0;
    v->len = (uint32_t)s->notes[0].ms * RATE / 1000;
    if (!v->len) v->len = 1;
    v->phase = 0;
    if (!v->lfsr) v->lfsr = 0x3F1u;
    v->hold = 0;
    v->start_ms = gos_time_ms();
    v->gen++;
    __asm__ volatile("" ::: "memory");
    v->active = true;
    return (v->gen << 4) | i;
}

int gos_audio_play(const gos_sfx_t *s, uint8_t vol) { return start(s, vol, false); }
int gos_audio_loop(const gos_sfx_t *s, uint8_t vol) { return start(s, vol, true); }

static voice_t *by_handle(int handle)
{
    if (handle < 0) return NULL;
    voice_t *v = &voices[handle & 15];
    if (v->gen != (handle >> 4)) return NULL;
    return v;
}

void gos_audio_stop(int handle)
{
    voice_t *v = by_handle(handle);
    if (v) v->active = false;
}

void gos_audio_set_vol(int handle, uint8_t vol)
{
    voice_t *v = by_handle(handle);
    if (v) v->vol = vol + 1;
}

void gos_audio_stop_all(void)
{
    for (int i = 0; i < NVOICES; i++) voices[i].active = false;
}

// --- render (audio task, core 0) -------------------------------------------

static int32_t voice_sample(voice_t *v)
{
    const gos_note_t *n = &v->seq[v->idx];
    float t = v->len > 1 ? (float)v->pos / (float)v->len : 0.0f;
    float freq = n->f0 + ((float)n->f1 - (float)n->f0) * t;
    v->phase += (uint32_t)(freq * (4294967296.0f / RATE));

    float env = 1.0f - 0.78f * t;
    if (v->pos < 44) env *= v->pos / 44.0f;
    float amp = VOICE_AMP * (n->vol / 255.0f) * env * (v->vol / 256.0f);

    int32_t out = 0;
    switch (n->wave) {
    case GOS_W_SQ50:
        out = (v->phase & 0x80000000u) ? (int32_t)amp : -(int32_t)amp;
        break;
    case GOS_W_SQ25:
        out = (v->phase >> 30) == 0 ? (int32_t)amp : -(int32_t)(amp * 0.55f);
        break;
    case GOS_W_TRI: {
        uint32_t p = v->phase >> 16;
        int32_t tri = p < 32768 ? (int32_t)p - 16384 : 49151 - (int32_t)p;
        out = (int32_t)(tri * amp / 16384.0f);
        break;
    }
    case GOS_W_SAW: {
        int32_t saw = (int32_t)(v->phase >> 16) - 32768;
        out = (int32_t)(saw * amp / 32768.0f);
        break;
    }
    case GOS_W_NOISE:
        if (--v->hold <= 0) {
            v->lfsr = (uint16_t)((v->lfsr >> 1) ^ (-(int)(v->lfsr & 1u) & 0xB400u));
            v->nval = (v->lfsr & 1u) ? 1 : -1;
            int h = (int)(RATE / (freq > 60.0f ? freq : 60.0f));
            v->hold = h > 0 ? h : 1;
        }
        out = (int32_t)(v->nval * amp);
        break;
    default:
        break;
    }

    if (++v->pos >= v->len) {
        v->idx++;
        v->pos = 0;
        v->phase = 0;
        if (v->idx >= v->count) {
            if (v->loop) v->idx = 0;
            else { v->active = false; return out; }
        }
        v->len = (uint32_t)v->seq[v->idx].ms * RATE / 1000;
        if (!v->len) v->len = 1;
    }
    return out;
}

bool gos_mixer_render(int16_t *out, int samples)
{
    bool any = false;
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].active) { any = true; break; }
    if (!any || master == 0) return false;

    float scale = level_scale[master];
    for (int s = 0; s < samples; s++) {
        int32_t acc = 0;
        for (int i = 0; i < NVOICES; i++)
            if (voices[i].active) acc += voice_sample(&voices[i]);
        acc = (int32_t)(acc * scale);
        if (acc > 32767) acc = 32767;
        if (acc < -32768) acc = -32768;
        out[s] = (int16_t)acc;
    }
    return true;
}
