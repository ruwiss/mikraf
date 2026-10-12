#pragma once
#include "settings.h"

#define MIKRAF_DSP_MAX_CHANNELS 8
#define MIKRAF_DSP_FRAME_CAP 960

typedef struct EqBand {
    float lf0, lf1, lf2, lf3;
    float hf0, hf1, hf2, hf3;
    float d1, d2, d3;
} EqBand;

typedef void *(*MikrafSpxOpen)(int frame, int rate);
typedef void (*MikrafSpxClose)(void *state);
typedef void (*MikrafSpxRun)(void *state, float *samples, int count, int suppressDb);

typedef struct Dsp {
    int rate;
    int channels;
    int frame;
    int maxFrames;
    int nsPrimed;
    void *spx[2];
    MikrafSpxRun spxRun;
    MikrafSpxClose spxClose;
    float hold[2][MIKRAF_DSP_FRAME_CAP];
    int filled;
    float *ring[2];
    int ringCap;
    int ringCount;
    int ringRead;
    float *planar[2];
    float *env;
    float *gain;
    float lf, hf;
    EqBand band[2];
    float compEnv;
    int compAttackMs, compReleaseMs;
    float compAttack, compRelease;
    float limEnv;
    int limReleaseMs;
    float limAttack, limRelease;
    float expRun[2], expGain[2];
    int expAttackMs, expReleaseMs;
    float expAttack, expRelease;
    float upRun[2], upGain[2];
    int upAttackMs, upReleaseMs;
    float upAttack, upRelease;
    float gateOpenV, gateCloseV, gateDecay, gateAttackC, gateReleaseC, gateHoldTime, gateSampleI;
    int gateAttackMs, gateHoldMs, gateReleaseMs;
    float gateOpenDb, gateCloseDb;
    int gateIsOpen;
    float gateAtten, gateLevel, gateHeld;
} Dsp;

int dsp_prepare(Dsp *d, int rate, int channels, int maxFrames, MikrafSpxOpen openFn, MikrafSpxClose closeFn, MikrafSpxRun runFn);
void dsp_release(Dsp *d);
void dsp_process(Dsp *d, const MikrafApoPayload *settings, const float *src, float *dst, int frames);
void mikraf_settings_default(MikrafApoPayload *settings);
