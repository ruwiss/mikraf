#include "dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float db_to_mul(float db)
{
    if (db <= -100.f)
        return 0.f;
    return powf(10.f, db / 20.f);
}

static float mul_to_db(float mul)
{
    if (!(mul > 0.f))
        return -1000.f;
    return 20.f * log10f(mul);
}

static float gain_coeff(int rate, float seconds)
{
    if (seconds <= 0.f || rate <= 0)
        return 0.f;
    return expf(-1.f / ((float)rate * seconds));
}

static int finite_sample(float x)
{
    return x == x && x < 1.0e20f && x > -1.0e20f;
}

void mikraf_settings_default(MikrafApoPayload *s)
{
    memset(s, 0, sizeof(*s));
    s->suppressDb = -10;
    s->expRatio = 3.f;
    s->expThreshold = -40.f;
    s->expOutput = 3.f;
    s->expAttack = 1;
    s->expRelease = 40;
    s->eqLow = 1.f;
    s->eqMid = -2.f;
    s->eqHigh = 3.f;
    s->compRatio = 3.f;
    s->compThreshold = -15.f;
    s->compAttack = 1;
    s->compRelease = 40;
    s->limThreshold = -2.f;
    s->limRelease = 60;
    s->gateOpen = -26.f;
    s->gateClose = -32.f;
    s->gateAttack = 25;
    s->gateHold = 200;
    s->gateRelease = 150;
    s->upRatio = 0.5f;
    s->upThreshold = -20.f;
    s->upAttack = 10;
    s->upRelease = 50;
    s->upKnee = 10;
    s->nsOn = 1;
    s->expOn = 1;
    s->eqOn = 1;
    s->compOn = 1;
    s->limOn = 1;
    s->chainCount = 5;
    s->chain[0] = MIKRAF_ST_NS;
    s->chain[1] = MIKRAF_ST_EXP;
    s->chain[2] = MIKRAF_ST_EQ;
    s->chain[3] = MIKRAF_ST_COMP;
    s->chain[4] = MIKRAF_ST_LIM;
}

static void clear_dynamics(Dsp *d)
{
    d->nsPrimed = 0;
    d->filled = 0;
    d->ringCount = 0;
    d->ringRead = 0;
    memset(d->band, 0, sizeof(d->band));
    d->compEnv = 0;
    d->compAttackMs = -1;
    d->limEnv = 0;
    d->limReleaseMs = -1;
    memset(d->expRun, 0, sizeof(d->expRun));
    memset(d->expGain, 0, sizeof(d->expGain));
    d->expAttackMs = -1;
    memset(d->upRun, 0, sizeof(d->upRun));
    memset(d->upGain, 0, sizeof(d->upGain));
    d->upAttackMs = -1;
    d->gateAttackMs = -1;
    d->gateIsOpen = 0;
    d->gateAtten = 0;
    d->gateLevel = 0;
    d->gateHeld = 0;
    if (d->rate > 0) {
        d->lf = 2.f * sinf(3.14159265f * 800.f / (float)d->rate);
        d->hf = 2.f * sinf(3.14159265f * 5000.f / (float)d->rate);
    }
}

int dsp_prepare(Dsp *d, int rate, int channels, int maxFrames, MikrafSpxOpen openFn, MikrafSpxClose closeFn, MikrafSpxRun runFn)
{
    int i;
    dsp_release(d);
    if (rate < 8000)
        rate = 48000;
    if (channels < 1)
        channels = 1;
    if (channels > MIKRAF_DSP_MAX_CHANNELS)
        channels = MIKRAF_DSP_MAX_CHANNELS;
    if (maxFrames < 256)
        maxFrames = 256;
    d->rate = rate;
    d->channels = channels;
    d->maxFrames = maxFrames;
    d->frame = rate / 100;
    if (d->frame < 1)
        d->frame = 1;
    if (d->frame > MIKRAF_DSP_FRAME_CAP)
        d->frame = 0;
    d->spxRun = runFn;
    d->spxClose = closeFn;
    d->ringCap = maxFrames + (d->frame > 0 ? d->frame : 480) + 8;
    for (i = 0; i < 2; i++) {
        d->ring[i] = (float *)calloc((size_t)d->ringCap, sizeof(float));
        d->planar[i] = (float *)calloc((size_t)maxFrames, sizeof(float));
        if (!d->ring[i] || !d->planar[i]) {
            dsp_release(d);
            return 0;
        }
    }
    d->env = (float *)calloc((size_t)maxFrames, sizeof(float));
    d->gain = (float *)calloc((size_t)maxFrames, sizeof(float));
    if (!d->env || !d->gain) {
        dsp_release(d);
        return 0;
    }
    if (d->frame > 0 && openFn && runFn) {
        for (i = 0; i < 2 && i < channels; i++)
            d->spx[i] = openFn(d->frame, rate);
    }
    clear_dynamics(d);
    return 1;
}

void dsp_release(Dsp *d)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (d->spx[i] && d->spxClose)
            d->spxClose(d->spx[i]);
        d->spx[i] = NULL;
        free(d->ring[i]);
        free(d->planar[i]);
        d->ring[i] = NULL;
        d->planar[i] = NULL;
    }
    free(d->env);
    free(d->gain);
    d->env = NULL;
    d->gain = NULL;
    d->spxRun = NULL;
    d->spxClose = NULL;
}

static int stage_on(const MikrafApoPayload *s, int code)
{
    switch (code) {
    case MIKRAF_ST_NS: return s->nsOn;
    case MIKRAF_ST_GATE: return s->gateOn;
    case MIKRAF_ST_INVERT: return s->invertOn;
    case MIKRAF_ST_GAIN: return s->gainOn;
    case MIKRAF_ST_EXP: return s->expOn;
    case MIKRAF_ST_UP: return s->upOn;
    case MIKRAF_ST_EQ: return s->eqOn;
    case MIKRAF_ST_COMP: return s->compOn;
    case MIKRAF_ST_LIM: return s->limOn;
    default: return 0;
    }
}

static void ring_push(Dsp *d, int count)
{
    int i, c, base;
    if (count <= 0 || d->ringCount + count > d->ringCap)
        return;
    base = (d->ringRead + d->ringCount) % d->ringCap;
    for (c = 0; c < 2; c++) {
        for (i = 0; i < count; i++)
            d->ring[c][(base + i) % d->ringCap] = d->hold[c][i];
    }
    d->ringCount += count;
}

static void ring_pop(Dsp *d, float **planar, int count)
{
    int i, c;
    if (count > d->ringCount)
        count = d->ringCount;
    for (c = 0; c < 2; c++) {
        for (i = 0; i < count; i++)
            planar[c][i] = d->ring[c][(d->ringRead + i) % d->ringCap];
    }
    d->ringRead = (d->ringRead + count) % d->ringCap;
    d->ringCount -= count;
}

static void apply_ns(Dsp *d, const MikrafApoPayload *s, float **planar, int frames)
{
    int inPos = 0;
    int channels = d->channels < 2 ? d->channels : 2;
    int suppress = s->suppressDb;
    if (d->frame <= 0 || !d->spx[0] || !d->spxRun)
        return;
    if (!d->nsPrimed) {
        memset(d->ring[0], 0, sizeof(float) * (size_t)d->ringCap);
        memset(d->ring[1], 0, sizeof(float) * (size_t)d->ringCap);
        d->ringRead = 0;
        d->ringCount = d->frame;
        d->filled = 0;
        d->nsPrimed = 1;
    }
    if (suppress > 0)
        suppress = 0;
    if (suppress < -60)
        suppress = -60;
    while (inPos < frames) {
        int space = d->frame - d->filled;
        int n = frames - inPos;
        int c, i;
        if (n > space)
            n = space;
        for (c = 0; c < channels; c++) {
            for (i = 0; i < n; i++)
                d->hold[c][d->filled + i] = planar[c][inPos + i];
        }
        if (channels == 1)
            memset(d->hold[1] + d->filled, 0, sizeof(float) * (size_t)n);
        d->filled += n;
        inPos += n;
        if (d->filled == d->frame) {
            for (c = 0; c < channels; c++)
                d->spxRun(d->spx[c], d->hold[c], d->frame, suppress);
            ring_push(d, d->frame);
            d->filled = 0;
        }
    }
    if (d->ringCount < frames) {
        int have = d->ringCount;
        ring_pop(d, planar, have);
        for (int c = 0; c < channels; c++)
            memset(planar[c] + have, 0, sizeof(float) * (size_t)(frames - have));
        return;
    }
    ring_pop(d, planar, frames);
}

static void sync_gate(Dsp *d, const MikrafApoPayload *s)
{
    float attackSec, releaseSec, minDecay;
    if (d->gateAttackMs == s->gateAttack && d->gateHoldMs == s->gateHold && d->gateReleaseMs == s->gateRelease
        && d->gateOpenDb == s->gateOpen && d->gateCloseDb == s->gateClose)
        return;
    d->gateAttackMs = s->gateAttack;
    d->gateHoldMs = s->gateHold;
    d->gateReleaseMs = s->gateRelease;
    d->gateOpenDb = s->gateOpen;
    d->gateCloseDb = s->gateClose;
    d->gateOpenV = db_to_mul(s->gateOpen);
    d->gateCloseV = db_to_mul(s->gateClose);
    d->gateSampleI = 1.f / (float)d->rate;
    attackSec = s->gateAttack > 0 ? s->gateAttack / 1000.f : 0.f;
    releaseSec = s->gateRelease > 0 ? s->gateRelease / 1000.f : 0.f;
    d->gateAttackC = attackSec <= 0.f ? 1.f : 1.f / (attackSec * (float)d->rate);
    d->gateReleaseC = releaseSec <= 0.f ? 1.f : 1.f / (releaseSec * (float)d->rate);
    minDecay = (1.f / 75.f) * (float)d->rate;
    d->gateDecay = minDecay > 0.f ? (d->gateOpenV - d->gateCloseV) / minDecay : 0.f;
    d->gateHoldTime = s->gateHold > 0 ? s->gateHold / 1000.f : 0.f;
    d->gateIsOpen = 0;
    d->gateAtten = 0;
    d->gateLevel = 0;
    d->gateHeld = 0;
}

static void apply_gate(Dsp *d, const MikrafApoPayload *s, float **planar, int frames)
{
    int channels = d->channels < 2 ? d->channels : 2;
    int i, c;
    sync_gate(d, s);
    for (i = 0; i < frames; i++) {
        float cur = 0.f;
        for (c = 0; c < channels; c++) {
            float a = fabsf(planar[c][i]);
            if (a > cur)
                cur = a;
        }
        if (cur > d->gateOpenV && !d->gateIsOpen)
            d->gateIsOpen = 1;
        if (d->gateLevel < d->gateCloseV && d->gateIsOpen) {
            d->gateHeld = 0;
            d->gateIsOpen = 0;
        }
        d->gateLevel = d->gateLevel > cur ? d->gateLevel : cur;
        d->gateLevel -= d->gateDecay;
        if (d->gateIsOpen)
            d->gateAtten = d->gateAtten + d->gateAttackC < 1.f ? d->gateAtten + d->gateAttackC : 1.f;
        else {
            d->gateHeld += d->gateSampleI;
            if (d->gateHeld > d->gateHoldTime)
                d->gateAtten = d->gateAtten - d->gateReleaseC > 0.f ? d->gateAtten - d->gateReleaseC : 0.f;
        }
        for (c = 0; c < channels; c++)
            planar[c][i] *= d->gateAtten;
    }
}

static void apply_eq(Dsp *d, const MikrafApoPayload *s, float **planar, int frames)
{
    const float epsilon = 1.f / 4294967295.f;
    float low = db_to_mul(s->eqLow);
    float mid = db_to_mul(s->eqMid);
    float high = db_to_mul(s->eqHigh);
    int channels = d->channels < 2 ? d->channels : 2;
    int c, i;
    for (c = 0; c < channels; c++) {
        EqBand b = d->band[c];
        float *row = planar[c];
        for (i = 0; i < frames; i++) {
            float sample = row[i];
            float l, h, m;
            b.lf0 += d->lf * (sample - b.lf0) + epsilon;
            b.lf1 += d->lf * (b.lf0 - b.lf1);
            b.lf2 += d->lf * (b.lf1 - b.lf2);
            b.lf3 += d->lf * (b.lf2 - b.lf3);
            l = b.lf3;
            b.hf0 += d->hf * (sample - b.hf0) + epsilon;
            b.hf1 += d->hf * (b.hf0 - b.hf1);
            b.hf2 += d->hf * (b.hf1 - b.hf2);
            b.hf3 += d->hf * (b.hf2 - b.hf3);
            h = b.d3 - b.hf3;
            m = b.d3 - (h + l);
            b.d3 = b.d2;
            b.d2 = b.d1;
            b.d1 = sample;
            row[i] = (l * low) + (m * mid) + (h * high);
        }
        d->band[c] = b;
    }
}

static void apply_comp(Dsp *d, const MikrafApoPayload *s, float **planar, int frames)
{
    int channels = d->channels < 2 ? d->channels : 2;
    float slope, threshold, output, env;
    int c, i;
    if (d->compAttackMs != s->compAttack || d->compReleaseMs != s->compRelease) {
        d->compAttackMs = s->compAttack;
        d->compReleaseMs = s->compRelease;
        d->compAttack = gain_coeff(d->rate, s->compAttack / 1000.f);
        d->compRelease = gain_coeff(d->rate, s->compRelease / 1000.f);
    }
    memset(d->env, 0, sizeof(float) * (size_t)frames);
    for (c = 0; c < channels; c++) {
        env = d->compEnv;
        for (i = 0; i < frames; i++) {
            float input = fabsf(planar[c][i]);
            env = input + (env < input ? d->compAttack : d->compRelease) * (env - input);
            if (env > d->env[i])
                d->env[i] = env;
        }
    }
    if (frames > 0)
        d->compEnv = d->env[frames - 1];
    slope = 1.f - (1.f / (s->compRatio > 1.f ? s->compRatio : 1.f));
    threshold = s->compThreshold;
    output = db_to_mul(s->compOutput);
    for (i = 0; i < frames; i++) {
        float gainDb = slope * (threshold - mul_to_db(d->env[i]));
        float gain = db_to_mul(gainDb < 0.f ? gainDb : 0.f) * output;
        for (c = 0; c < channels; c++)
            planar[c][i] *= gain;
    }
}

static void apply_lim(Dsp *d, const MikrafApoPayload *s, float **planar, int frames)
{
    int channels = d->channels < 2 ? d->channels : 2;
    float env;
    int c, i;
    int release = s->limRelease > 1 ? s->limRelease : 1;
    if (d->limReleaseMs != release) {
        d->limReleaseMs = release;
        d->limAttack = gain_coeff(d->rate, 0.001f / 1000.f);
        d->limRelease = gain_coeff(d->rate, release / 1000.f);
    }
    memset(d->env, 0, sizeof(float) * (size_t)frames);
    for (c = 0; c < channels; c++) {
        env = d->limEnv;
        for (i = 0; i < frames; i++) {
            float input = fabsf(planar[c][i]);
            env = input + (env < input ? d->limAttack : d->limRelease) * (env - input);
            if (env > d->env[i])
                d->env[i] = env;
        }
    }
    if (frames > 0)
        d->limEnv = d->env[frames - 1];
    for (i = 0; i < frames; i++) {
        float gainDb = s->limThreshold - mul_to_db(d->env[i]);
        float gain = db_to_mul(gainDb < 0.f ? gainDb : 0.f);
        for (c = 0; c < channels; c++)
            planar[c][i] *= gain;
    }
}

static void apply_dyn(Dsp *d, float **planar, int frames, int upward, float ratio, float threshold, float outputDb,
    int attackMs, int releaseMs, int detector, int knee)
{
    int channels = d->channels < 2 ? d->channels : 2;
    float *run = upward ? d->upRun : d->expRun;
    float *state = upward ? d->upGain : d->expGain;
    int *attackSlot = upward ? &d->upAttackMs : &d->expAttackMs;
    int *releaseSlot = upward ? &d->upReleaseMs : &d->expReleaseMs;
    float *attackC = upward ? &d->upAttack : &d->expAttack;
    float *releaseC = upward ? &d->upRelease : &d->expRelease;
    float slope = 1.f - ratio;
    float output = db_to_mul(outputDb);
    float rms, invAttack, invRelease;
    int c, i;
    if (attackMs < 1)
        attackMs = 1;
    if (releaseMs < 1)
        releaseMs = 1;
    if (*attackSlot != attackMs || *releaseSlot != releaseMs) {
        *attackSlot = attackMs;
        *releaseSlot = releaseMs;
        *attackC = gain_coeff(d->rate, attackMs / 1000.f);
        *releaseC = gain_coeff(d->rate, releaseMs / 1000.f);
    }
    rms = powf(2.f, -100.f / (float)d->rate);
    invAttack = 1.f - *attackC;
    invRelease = 1.f - *releaseC;
    for (c = 0; c < channels; c++) {
        float runV = run[c];
        float *row = planar[c];
        if (detector == 0) {
            for (i = 0; i < frames; i++) {
                float sample = row[i];
                runV = (rms * runV) + ((1.f - rms) * sample * sample);
                d->env[i] = sqrtf(runV > 0.f ? runV : 0.f);
            }
        } else {
            for (i = 0; i < frames; i++)
                d->env[i] = fabsf(row[i]);
        }
        run[c] = runV;
        for (i = 0; i < frames; i++) {
            float envDb = mul_to_db(d->env[i]);
            float diff = threshold - envDb;
            float prev = i > 0 ? d->gain[i - 1] : state[c];
            float target;
            float linear;
            if (upward && envDb <= (threshold - 60.f) / 2.f)
                diff = envDb + 60.f > 0.f ? envDb + 60.f : 0.f;
            if (upward) {
                float kneeF = (float)knee;
                prev = prev > 0.f ? prev : 0.f;
                if (kneeF <= 0.0001f)
                    target = envDb >= threshold ? 0.f : slope * diff;
                else if (envDb >= threshold + (kneeF / 2.f))
                    target = 0.f;
                else if (threshold - (kneeF / 2.f) >= envDb)
                    target = slope * diff;
                else
                    target = slope * powf(diff + (kneeF / 2.f), 2.f) / (2.f * kneeF);
            } else {
                target = diff > 0.f ? (slope * diff > -60.f ? slope * diff : -60.f) : 0.f;
            }
            d->gain[i] = target > prev ? (*attackC * prev) + (invAttack * target) : (*releaseC * prev) + (invRelease * target);
            linear = upward ? db_to_mul(d->gain[i]) : db_to_mul(d->gain[i] < 0.f ? d->gain[i] : 0.f);
            row[i] *= linear * output;
        }
        if (frames > 0)
            state[c] = d->gain[frames - 1];
    }
}

void dsp_process(Dsp *d, const MikrafApoPayload *settings, const float *src, float *dst, int frames)
{
    int channels = d->channels;
    int used = channels < 2 ? channels : 2;
    int i, c, k;
    MikrafApoPayload local;
    if (!d->planar[0] || frames <= 0 || frames > d->maxFrames) {
        if (src != dst && frames > 0)
            memcpy(dst, src, sizeof(float) * (size_t)frames * (size_t)channels);
        return;
    }
    if (!settings) {
        mikraf_settings_default(&local);
        settings = &local;
    }
    for (i = 0; i < frames; i++) {
        for (c = 0; c < used; c++) {
            float x = src[(i * channels) + c];
            d->planar[c][i] = finite_sample(x) ? x : 0.f;
        }
    }
    if (!settings->bypass) {
        int count = settings->chainCount;
        if (count > 12)
            count = 12;
        if (count < 0)
            count = 0;
        for (k = 0; k < count; k++) {
            int code = settings->chain[k];
            if (!stage_on(settings, code))
                continue;
            switch (code) {
            case MIKRAF_ST_NS:
                apply_ns(d, settings, d->planar, frames);
                break;
            case MIKRAF_ST_GATE:
                apply_gate(d, settings, d->planar, frames);
                break;
            case MIKRAF_ST_INVERT:
                for (c = 0; c < used; c++)
                    for (i = 0; i < frames; i++)
                        d->planar[c][i] = -d->planar[c][i];
                break;
            case MIKRAF_ST_GAIN: {
                float mul = db_to_mul(settings->gainDb);
                for (c = 0; c < used; c++)
                    for (i = 0; i < frames; i++)
                        d->planar[c][i] *= mul;
                break;
            }
            case MIKRAF_ST_EXP:
                apply_dyn(d, d->planar, frames, 0, settings->expRatio, settings->expThreshold, settings->expOutput,
                    settings->expAttack, settings->expRelease, settings->expDetector, 0);
                break;
            case MIKRAF_ST_UP:
                apply_dyn(d, d->planar, frames, 1, settings->upRatio, settings->upThreshold, settings->upOutput,
                    settings->upAttack, settings->upRelease, settings->upDetector, settings->upKnee);
                break;
            case MIKRAF_ST_EQ:
                apply_eq(d, settings, d->planar, frames);
                break;
            case MIKRAF_ST_COMP:
                apply_comp(d, settings, d->planar, frames);
                break;
            case MIKRAF_ST_LIM:
                apply_lim(d, settings, d->planar, frames);
                break;
            default:
                break;
            }
        }
    }
    for (i = 0; i < frames; i++) {
        for (c = 0; c < used; c++) {
            float y = d->planar[c][i];
            dst[(i * channels) + c] = finite_sample(y) ? y : 0.f;
        }
        for (c = used; c < channels; c++)
            dst[(i * channels) + c] = src[(i * channels) + c];
    }
    if (settings->bypass || !settings->nsOn)
        d->nsPrimed = 0;
}
