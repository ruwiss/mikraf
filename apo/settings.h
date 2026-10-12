#pragma once
/* Shared with ApoBridge.cs. Keep the field order and Pack=4 layout identical. */
#include <stdint.h>

#pragma pack(push, 4)
typedef struct MikrafApoPayload {
    int32_t bypass;
    int32_t suppressDb;
    int32_t method;
    float expRatio;
    float expThreshold;
    float expOutput;
    int32_t expAttack;
    int32_t expRelease;
    int32_t expDetector;
    float eqLow;
    float eqMid;
    float eqHigh;
    float compRatio;
    float compThreshold;
    float compOutput;
    int32_t compAttack;
    int32_t compRelease;
    float limThreshold;
    int32_t limRelease;
    float gainDb;
    float gateOpen;
    float gateClose;
    int32_t gateAttack;
    int32_t gateHold;
    int32_t gateRelease;
    float upRatio;
    float upThreshold;
    float upOutput;
    int32_t upKnee;
    int32_t upAttack;
    int32_t upRelease;
    int32_t upDetector;
    int32_t nsOn;
    int32_t gateOn;
    int32_t invertOn;
    int32_t gainOn;
    int32_t expOn;
    int32_t upOn;
    int32_t eqOn;
    int32_t compOn;
    int32_t limOn;
    int32_t chainCount;
    int32_t chain[12];
} MikrafApoPayload;
#pragma pack(pop)

#define MIKRAF_APO_MAGIC 0x314F5041u
#define MIKRAF_APO_VERSION 1

#define MIKRAF_ST_NS 1
#define MIKRAF_ST_GATE 2
#define MIKRAF_ST_INVERT 3
#define MIKRAF_ST_GAIN 4
#define MIKRAF_ST_EXP 5
#define MIKRAF_ST_UP 6
#define MIKRAF_ST_EQ 7
#define MIKRAF_ST_COMP 8
#define MIKRAF_ST_LIM 9

typedef struct MikrafApoBlock {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;
    MikrafApoPayload payload;
    volatile uint32_t processCount;
    volatile uint32_t sampleRate;
    volatile uint32_t channelCount;
} MikrafApoBlock;
